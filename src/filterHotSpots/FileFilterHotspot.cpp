/*
    Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>
    Copyright 2020 by Tomaz Canabrava <tcanabrava@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA.
*/

#include "FileFilterHotspot.h"

#include <QApplication>
#include <QAction>
#include <QBuffer>
#include <QClipboard>
#include <QMenu>
#include <QTimer>
#include <QToolTip>

#include <KIO/CommandLauncherJob>
#include <KIO/JobUiDelegate>
#include <KRun>
#include <KLocalizedString>
#include <KFileItemListProperties>

#include "konsoledebug.h"
#include "KonsoleSettings.h"

using namespace Konsole;


FileFilterHotSpot::FileFilterHotSpot(int startLine, int startColumn, int endLine, int endColumn,
                             const QStringList &capturedTexts, const QString &filePath) :
    RegExpFilterHotSpot(startLine, startColumn, endLine, endColumn, capturedTexts),
    _filePath(filePath)
{
    setType(Link);
}

void FileFilterHotSpot::activate(QObject *)
{
    QString editorCmd(QStringLiteral("/usr/bin/kate --line LINE_NUMBER --column COLUMN_NUMBER"));
    // Output of e.g.:
    // grep with line numbers: "path/to/some/file:123:"
    // or compiler errors with line/column numbers: "/path/to/file.cpp:123:123:"
    const auto re(QRegularExpression(QStringLiteral(R"foo((:([0-9]+):)(([0-9]+):)?$)foo")));
    const QRegularExpressionMatch match = re.match(_filePath);
    if (match.hasMatch()) {
        editorCmd.replace(QLatin1String("LINE_NUMBER"), match.captured(2));

        QString col = match.captured(4);
        if (col.isEmpty()) {
            col = QLatin1Char('0');
        }
        editorCmd.replace(QLatin1String("COLUMN_NUMBER"), col);

        editorCmd += QLatin1Char(' ') + _filePath.left(match.capturedStart(0));
        auto *job = new KIO::CommandLauncherJob(editorCmd);
        job->setUiDelegate(new KDialogJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, QApplication::activeWindow()));
        job->start();
        return;
    }

    new KRun(QUrl::fromLocalFile(_filePath), QApplication::activeWindow());
}


FileFilterHotSpot::~FileFilterHotSpot() = default;

QList<QAction *> FileFilterHotSpot::actions()
{
    QAction *action = new QAction(i18n("Copy Location"), this);
    action->setIcon(QIcon::fromTheme(QStringLiteral("edit-copy")));
    connect(action, &QAction::triggered, this, [this] {
        QGuiApplication::clipboard()->setText(_filePath);
    });
    return {action};
}

void FileFilterHotSpot::setupMenu(QMenu *menu)
{
    // We are reusing the QMenu, but we need to update the actions anyhow.
    // Remove the 'Open with' actions from it, then add the new ones.
    QList<QAction*> toDelete;
    for (auto *action : menu->actions()) {
        if (action->text().toLower().remove(QLatin1Char('&')).contains(i18n("open with"))) {
            toDelete.append(action);
        }
    }
    qDeleteAll(toDelete);

    const KFileItem fileItem(QUrl::fromLocalFile(_filePath));
    const KFileItemList itemList({fileItem});
    const KFileItemListProperties itemProperties(itemList);
    _menuActions.setParent(this);
    _menuActions.setItemListProperties(itemProperties);
    _menuActions.addOpenWithActionsTo(menu);

    // Here we added the actions to the last part of the menu, but we need to move them up.
    // TODO: As soon as addOpenWithActionsTo accepts a index, change this.
    // https://bugs.kde.org/show_bug.cgi?id=423765
    QAction *firstAction = menu->actions().at(0);
    for (auto *action : menu->actions()) {
        if (action->text().toLower().remove(QLatin1Char('&')).contains(i18n("open with"))) {
            menu->removeAction(action);
            menu->insertAction(firstAction, action);
        }
    }
    auto *separator = new QAction(this);
    separator->setSeparator(true);
    menu->insertAction(firstAction, separator);
}

// Static variables for the HotSpot
qintptr FileFilterHotSpot::currentThumbnailHotspot = 0;
bool FileFilterHotSpot::_canGenerateThumbnail = false;
QPointer<KIO::PreviewJob> FileFilterHotSpot::_previewJob;

void FileFilterHotSpot::requestThumbnail(Qt::KeyboardModifiers modifiers, const QPoint &pos) {
    _canGenerateThumbnail = true;
    currentThumbnailHotspot = reinterpret_cast<qintptr>(this);
    _eventModifiers = modifiers;
    _eventPos = pos;

    // Defer the real creation of the thumbnail by a few msec.
    QTimer::singleShot(250, this, [this]{
        if (currentThumbnailHotspot != reinterpret_cast<qintptr>(this)) {
            return;
        }

        thumbnailRequested();
    });
}

void FileFilterHotSpot::stopThumbnailGeneration()
{
    _canGenerateThumbnail = false;
    if (_previewJob != nullptr) {
        _previewJob->deleteLater();
        QToolTip::hideText();
    }
}

void FileFilterHotSpot::showThumbnail(const KFileItem& item, const QPixmap& preview)
{
    if (!_canGenerateThumbnail) {
        return;
    }
    _thumbnailFinished = true;
    Q_UNUSED(item)
    QByteArray data;
    QBuffer buffer(&data);
    preview.save(&buffer, "PNG", 100);

    const auto tooltipString = QStringLiteral("<img src='data:image/png;base64, %0'>")
        .arg(QString::fromLocal8Bit(data.toBase64()));

    QToolTip::showText(_thumbnailPos, tooltipString, qApp->focusWidget());
}

void FileFilterHotSpot::thumbnailRequested() {
    if (!_canGenerateThumbnail) {
        return;
    }

    auto *settings = KonsoleSettings::self();

    Qt::KeyboardModifiers modifiers = settings->thumbnailCtrl() ? Qt::ControlModifier : Qt::NoModifier;
    modifiers |= settings->thumbnailAlt() ? Qt::AltModifier : Qt::NoModifier;
    modifiers |= settings->thumbnailShift() ? Qt::ShiftModifier : Qt::NoModifier;

    if (_eventModifiers != modifiers) {
        return;
    }

    _thumbnailPos = QPoint(_eventPos.x() + 100, _eventPos.y() - settings->thumbnailSize() / 2);

    const int size = KonsoleSettings::thumbnailSize();
    if (_previewJob != nullptr) {
        _previewJob->deleteLater();
    }

    _thumbnailFinished = false;

    // Show a "Loading" if Preview takes a long time.
    QTimer::singleShot(10, this, [this]{
        if (_previewJob == nullptr) {
            return;
        }
        if (!_thumbnailFinished) {
            QToolTip::showText(_thumbnailPos, i18n("Generating Thumbnail"), qApp->focusWidget());
        }
    });

    _previewJob = new KIO::PreviewJob(KFileItemList({fileItem()}), QSize(size, size));
    connect(_previewJob, &KIO::PreviewJob::gotPreview, this, &FileFilterHotSpot::showThumbnail);
    connect(_previewJob, &KIO::PreviewJob::failed, this, []{
        qCDebug(KonsoleDebug) << "Error generating the preview" << _previewJob->errorString();
        QToolTip::hideText();
    });

    _previewJob->setAutoDelete(true);
    _previewJob->start();
}

KFileItem FileFilterHotSpot::fileItem() const
{
    return KFileItem(QUrl::fromLocalFile(_filePath));
}
