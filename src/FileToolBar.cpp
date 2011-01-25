/*
    YAGF - cuneiform OCR graphical front-end
    Copyright (C) 2009-2010 Andrei Borovsky <anb@symmetrica.net>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "FileToolBar.h"
#include <QSize>
#include <QAction>
#include <QIcon>
#include <QPixmap>
#include <QPushButton>
#include <QFileDialog>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QMatrix>
#include <QMultiMap>
#include <QRect>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include "utils.h"


FileToolBar::FileToolBar(QWidget * parent):QToolBar(trUtf8("Loaded Images"), parent)
{
    buttonsAdded = false;
    this->setIconSize(QSize(64, 96));
    this->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    filesMap = new StringMap();
    rotMap = new IntMap();
    scaleMap = new FloatMap();
    blocksMap = new RectMap();
    saveButton = NULL;
    clearButton = NULL;
    removeButton = NULL;
    currentImage = "";
    setAcceptDrops(true);
    this->setToolTip(trUtf8("Drop graphic files here"));
    setStyleSheet(" FileToolBar { color: white; background-color: #444036 } \r QToolButton {color : white; background-color: #444036}");
 }

FileToolBar::~FileToolBar()
{
        delete filesMap;
        delete rotMap;
        delete scaleMap;
        delete blocksMap;
}

void FileToolBar::addFile(const QString &name)
{
    QPixmap pm;
    if (pm.load(name)) {
        addFile(pm, name);
    }
}

void FileToolBar::addFile(const QPixmap & pixmap, const QString & name)
{
    QAction * action;
    if (filesMap->values().contains(name))
        return;
    if (!buttonsAdded) {
        clearButton = new QPushButton(QIcon(":/clear.png"), trUtf8("Clear"), this);
        clearButton->setIconSize(QSize(24,24));
        clearButton->setToolTip(trUtf8("Clear the panel"));
        connect(clearButton, SIGNAL(clicked()), this, SLOT(clearAll()));
        action = insertWidget(0, clearButton);
        saveButton = new QPushButton(QIcon(":/save_all.png"), trUtf8("Save..."), this);
        saveButton->setIconSize(QSize(24,24));
        saveButton->setToolTip(trUtf8("Save all the files"));
        connect(saveButton, SIGNAL(clicked()), this, SLOT(saveAll()));
        insertWidget(action, saveButton);
        removeButton = new QPushButton(QIcon(":/remove.png"), trUtf8("Remove"), this);
        removeButton->setIconSize(QSize(24,24));
        removeButton->setToolTip(trUtf8("Remove the current file"));
        connect(removeButton, SIGNAL(clicked()), this, SLOT(remove()));
        insertWidget(action, removeButton);
        this->insertSeparator(0);
        buttonsAdded = true;
    }
    QPixmap pm = pixmap.scaledToHeight(96, Qt::FastTransformation);
    QString fn = extractFileName(name);
    if (filesMap->contains(fn))
        fn = fn.replace(".", "-1.");
    filesMap->insert(fn, name);
    rotMap->insert(fn, 0);
    action = addAction(QIcon(pm), fn);
    connect(action, SIGNAL(triggered()), this, SLOT(selected()));
    currentImage = fn;
}

void FileToolBar::selected()
{
    QString key = ((QAction *) sender())->text();
    currentImage = key;
    QString path = filesMap->value(key);
    emit fileSelected(path);
}

void FileToolBar::saveAll()
{
    QFileDialog fd(this, trUtf8("Select a directory"), QDir::home().path());
    fd.setFileMode(QFileDialog::DirectoryOnly);
    int rot = 0;
    QPixmap pm;
    if (fd.exec()) {
        QFile file;
        QMapIterator<QString, QString> i(*filesMap);
        while (i.hasNext()) {
             i.next();
             file.setFileName(i.value());
             rot = getRotation(i.value());
             if (rot) {
                pm.load(i.value());
                if (!pm.isNull()) {
                    QMatrix matrix;
                    matrix.rotate(rot);
                    pm = pm.transformed(matrix, Qt::SmoothTransformation);
                }
             }
             QString newName = fd.selectedFiles().at(0) + '/' + extractFileName(i.value());
             if (file.exists(newName)) {
                    QPixmap icon;
                    icon.load(":/warning.png");
                    QMessageBox messageBox(QMessageBox::NoIcon, "YAGF", trUtf8("File %1 already exists. Do you want to replace it?").arg(newName),
                        QMessageBox::Yes|QMessageBox::No, this);
                    messageBox.setIconPixmap(icon);
                    int result = messageBox.exec();
                    if (result == QMessageBox::No)
                        continue;
                    else
                        file.remove(newName);
             }
             if (rot && (!pm.isNull()))
                 pm.save(newName);
             else
                file.copy(newName);
         }

    }

}

void FileToolBar::clearAll()
{
    delete saveButton;
    delete clearButton;
    clear();
    buttonsAdded = false;
    filesMap->clear();
    rotMap->clear();
    scaleMap->clear();
    blocksMap->clear();
}

QStringList FileToolBar::getFileNames()
{
    QStringList S;
    QMapIterator<QString, QString> i(*filesMap);
    while (i.hasNext()) {
        i.next();
        S << i.value();
    }
    return S;
}

void FileToolBar::remove()
{
    if (currentImage == "")
        return;
    for (int i = 0; i < actions().count(); i++)
        if (actions().at(i)->text() == currentImage) {
            this->removeAction(actions().at(i));
            filesMap->remove(currentImage);
            rotMap->remove(currentImage);
            blocksMap->remove(currentImage);
            scaleMap->remove(currentImage);
            currentImage = "";
            if (filesMap->count() > 0) {
                currentImage = filesMap->keys().at(0);
                QString path = filesMap->value(currentImage);
                emit fileSelected(path);
            }
            break;
        }
}

void FileToolBar::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls())
        setCursor(Qt::ForbiddenCursor);
    event->accept();
}

void FileToolBar::dragLeaveEvent(QDragLeaveEvent *event)
{
    setCursor(Qt::ArrowCursor);
    event->accept();
}

void FileToolBar::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        QList<QUrl> ul = event->mimeData()->urls();
        QList<QUrl>::Iterator i;
        for(i = ul.begin(); i != ul.end(); i++) {
           QUrl url = *i;
           this->addFile(url.toLocalFile());
        }
    }
    setCursor(Qt::ArrowCursor);
    event->accept();
}

void FileToolBar::setRotation(const QString &name, int r)
{
    if (name != "") {
        rotMap->remove(name);
        rotMap->insert(name, r % 360);
    }

}


void FileToolBar::setBlock(const QRectF &block)
{
    if (currentImage != "")
        blocksMap->insertMulti(currentImage, block);
}

void FileToolBar::setBlock(const QString &name, const QRectF &block)
{
    if (currentImage != "")
        blocksMap->insertMulti(name, block);
}

void FileToolBar::removeBlock(const QRectF &block)
{
    if (currentImage != "")  {
        RectMap::iterator i;
        for (i = blocksMap->begin(); i != blocksMap->end(); i++) {
            QRectF rect = *i;
            if (rect == block) {
                blocksMap->values(currentImage).removeOne(block);
                return;
            }
        }
    }
}

QRectF FileToolBar::getBlock(int index)
{
    if (currentImage != "")
        return blocksMap->values(currentImage).value(index, QRectF(0,0,0,0));
    return QRectF(0,0,0,0);
}

int FileToolBar::getBlocksCount()
{
    if (currentImage != "")
        return blocksMap->values(currentImage).count();
    return 0;
}

void FileToolBar::setRotation(int r)
{
    if (currentImage != "") {
        rotMap->remove(currentImage);
        rotMap->insert(currentImage, r % 360);
    }
}

void FileToolBar::setScale(float s)
{
    if (currentImage != "") {
        scaleMap->remove(currentImage);
        scaleMap->insert(currentImage, s);
    }
}

int FileToolBar::getRotation()
{
    if (currentImage == "")
        return 0;
    return rotMap->value(currentImage);
}

float FileToolBar::getScale()
{
    if (currentImage == "")
        return 0;
    return scaleMap->value(currentImage, float(0));
}

float FileToolBar::getScale(const QString &name)
{
    if (name == "")
        return 0;
    QString internal = filesMap->keys(name).first();
    return scaleMap->value(internal, float(0));
}


int FileToolBar::getRotation(const QString &name)
{
    if (name == "")
        return 0;
    QString internal = filesMap->keys(name).first();
    return rotMap->value(internal);
}

bool FileToolBar::fileLoaded(const QString &name)
{
    return filesMap->values().contains(name);
}

void FileToolBar::select(const QString &name)
{
    currentImage = filesMap->keys(name).first();
}

void FileToolBar::selectFirstFile()
{
    if (filesMap->values().count() > 0)
        currentImage = filesMap->values().at(0);
}

void FileToolBar::addBlock(const QRect &rect)
{
    if (currentImage == "")
        return;
    blocksMap->insert(currentImage, rect);
}

/*RectList FileToolBar::getBlocks()
{
    return blocksMap->values(currentImage);
}*/

void FileToolBar::clearBlocks()
{
    blocksMap->remove(currentImage);
}
