/*
    YAGF - cuneiform and tesseract OCR graphical front-ends
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

#include "src/sidebar.h"
#include "src/qsnippet.h"
#include <QStringList>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QUrl>
#include <QFileInfo>
#include <QDrag>


SideBar::SideBar(QWidget *parent) :
    QListWidget(parent)
{
    //setDragDropOverwriteMode(true);
    current = 0;
    setMaximumWidth(120);
    setMinimumWidth(120);
    connect(this, SIGNAL(itemActivated(QListWidgetItem*)), this, SLOT(itemActive(QListWidgetItem*)));
    setToolTip(trUtf8("Drop files here"));
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    lock = false;
}

void SideBar::addFile(const QString &name, const QImage *image, bool select)
{
    if (getFileNames().contains(name))
            return;
    if (!image) {
        QImage img;
        if (img.load(name)) {
            QSnippet * snippet = new QSnippet(this);
            snippet->addFile(name, &img);
            current = snippet;
            current->setSelected(select);
        }
    } else {
        QSnippet * snippet = new QSnippet(this);
        snippet->addFile(name, image);
        current = snippet;
        current->setSelected(select);
    }
}

QStringList SideBar::getFileNames()
{
    QStringList sl;
    for (int i = 0; i < count(); i++) {
        sl.append(((QSnippet *)item(i))->getName());
    }
    return sl;
}

void SideBar::itemActive(QListWidgetItem *item)
{
    if (lock) return;
    lock = true;
    QString s = ((QSnippet *)item)->getName();
    emit fileSelected(s);
    current = ((QSnippet *)item);
    lock = false;
}

/*void SideBar::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls()) {
        setCursor(Qt::ForbiddenCursor);
        event->ignore();
    //     >mimeData()->setData("application/x-qabstractitemmodeldatalist", QByteArray());
    }
    else {
        setCursor(Qt::DragCopyCursor);
        event->accept();
    }
    QListWidget::dragEnterEvent(event);
}*/

QStringList SideBar::mimeTypes() const
{
    QStringList qstrList;
     qstrList.append("text/uri-list");
     return qstrList;
}

Qt::DropActions SideBar::supportedDropActions() const
{
    return Qt::CopyAction | Qt::MoveAction;
}

bool SideBar::dropMimeData(int index, const QMimeData *data, Qt::DropAction action)
{
    QList <QUrl> urlList;
       QSnippet * snippet;
       QFileInfo info;
       QString fName;

       urlList = data->urls(); // retrieve list of urls

       foreach(QUrl url, urlList) // iterate over list
       {
           fName = url.toLocalFile();
           info.setFile( fName );

           snippet = new QSnippet(0);
           if (snippet->addFile(fName))
               insertItem(index, snippet);
           else
               delete snippet;
           ++index; // increment index to preserve drop order
       }

       return true;
}

void SideBar::startDrag(Qt::DropActions supportedActions)
{
        supportedActions |= Qt::MoveAction;
        QDrag * drag = new QDrag(this);
        QMimeData *mimeData = new QMimeData();
        QList <QUrl> urlList;
        QString s = "";
        foreach(QListWidgetItem * lwi, selectedItems()) {
            QFileInfo fi(((QSnippet *)lwi)->getName());
            urlList << fi.canonicalFilePath();
            s = s + fi.canonicalFilePath() + ";";
        }
        mimeData->setUrls(urlList);
        mimeData->setText(s);
        drag->setMimeData(mimeData);
        if (drag->exec(supportedActions,Qt::CopyAction) == Qt::MoveAction) {
            foreach(QListWidgetItem * lwi, selectedItems())
                model()->removeRow(row(lwi));
        }
        if (selectedItems().count()) {
            emit fileSelected(((QSnippet *)selectedItems().at(0))->getName());
        } else {
            if (getFileNames().count())
                emit fileSelected(getFileNames().at(0));
        }
        //QListWidget::startDrag(supportedActions);
        //delete mimeData;
        //delete drag;
       // this->sortItems();
        current = 0;
}

QSnippet * SideBar::getItemByName(const QString &name)
{
    for (int i = 0; i < count(); i++) {
        if (((QSnippet *)item(i))->getName() == name)
            return ((QSnippet *)item(i));
    }
    return NULL;
}

void SideBar::setRotation(qreal r, const QString &name)
{
    if (name == "") {
        if (current) {
            current->setRotation(r);
        }
    } else {
        if (getItemByName(name))
            getItemByName(name)->setRotation(r);
    }
}

qreal SideBar::getRotation(const QString &name)
{
    if (name == "") {
        if (current) {
            return current->getRotation();
        }
    } else {
        if (getItemByName(name))
            return getItemByName(name)->getRotation();
    }
    return 0;
}

void SideBar::addBlock(const QRect &block, const QString &name)
{
    if (name == "") {
        if (current) {
            current->blocks()->append(block);
        }
    } else {
        if (getItemByName(name))
            getItemByName(name)->blocks()->append(block);
    }
}

QRect SideBar::getBlock(int index)
{
    if (current) {
        if (index < current->blocks()->count())
            return current->blocks()->at(index);
    }
    return QRect();
}

int SideBar::getBlocksCount()
{
    if (current)
        return current->blocks()->count();
    return 0;
}

void SideBar::clearBlocks()
{
    if (current)
        current->blocks()->clear();
}

void SideBar::removeBlock(const QRect &block)
{
    if (current)
        current->blocks()->removeOne(block);
}

void SideBar::setScale(float s)
{
    if (current)
        current->setScale(s);
}

float SideBar::getScale(const QString &name)
{
    if (name == "") {
        if (current) {
            return current->getScale();
        }
    } else {
        if (getItemByName(name))
            return getItemByName(name)->getScale();
    }
    return 1;
}

bool SideBar::fileLoaded(const QString &name)
{
    return getItemByName(name) != NULL;
}

void SideBar::select(const QString &name)
{
    current = getItemByName(name);
}

void SideBar::selectFirstFile()
{
    if (count() == 0) {
        current = NULL;
        return;
    }
    current = (QSnippet *) item(0);
}

void SideBar::setCrop1(const QRect &rect)
{
    if (current)
        current->setCrop1(rect);
}

void SideBar::setCrop2(const QRect &rect)
{
    if (current)
        current->setCrop2(rect);
}

QRect SideBar::getCrop1(const QString &name)
{
    if (name == "") {
        if (current) {
            return current->getCrop1();
        }
    } else {
        if (getItemByName(name))
            return getItemByName(name)->getCrop1();
    }
    return QRect(0,0,0,0);
}

QRect SideBar::getCrop2(const QString &name)
{
    if (name == "") {
        if (current) {
            return current->getCrop2();
        }
    } else {
        if (getItemByName(name))
            return getItemByName(name)->getCrop2();
    }
    return QRect(0,0,0,0);
}

QRect SideBar::getBlockByHalf(int index)
{
    if (current) {
        if (index < current->blocks()->count()) {
            QRect b = current->blocks()->at(index);
            return QRect(b.x()/2, b.y()/2,b.width()/2, b.height()/2);
        }
    }
    return QRect();
}

/*void SideBar::dragLeaveEvent(QDragLeaveEvent *event)
{
    setCursor(Qt::ArrowCursor);
    event->accept();
    //QListWidget::dragLeaveEvent(event);
}

void SideBar::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        QList<QUrl> ul = event->mimeData()->urls();
        QList<QUrl>::Iterator i;
        for (i = ul.begin(); i != ul.end(); i++) {
            QUrl url = *i;
            this->addFile(url.toLocalFile());
        }
    }
    setCursor(Qt::ArrowCursor);
    event->accept();
    //QListWidget::dropEvent(event);
}*/
