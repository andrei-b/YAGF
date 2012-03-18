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

#ifndef SIDEBAR_H
#define SIDEBAR_H

#include <QListWidget>

class QSnippet;

class SideBar : public QListWidget
{
    Q_OBJECT
public:
    explicit SideBar(QWidget *parent = 0);
    void addFile(const QString &name, const QImage * image = 0, bool select = true);
    QStringList getFileNames();
    void setRotation(qreal r, const QString &name = "");
    qreal getRotation(const QString &name = "");
    int getBlocksCount();
    void clearBlocks();
    void addBlock(const QRect &block, const QString &name = "");
    QRect getBlock(int index);
    QRect getBlockByHalf(int index);
    void removeBlock(const QRect &block);
    void setScale(float s);
    float getScale(const QString &name = "");
    void setCrop1(const QRect &rect);
    QRect getCrop1(const QString &name = "");
    void setCrop2(const QRect &rect);
    QRect getCrop2(const QString &name = "");
    bool fileLoaded(const QString &name);
    void select(const QString &name);
    void selectFirstFile();
signals:
    void fileSelected(const QString &name);
private slots:
    void itemActive( QListWidgetItem * item );
protected:
    QStringList mimeTypes () const;
    Qt::DropActions supportedDropActions () const;
    bool dropMimeData(int index, const QMimeData *data, Qt::DropAction action);
    void startDrag(Qt::DropActions supportedActions);

    //bool dropMimeData(int index, const QMimeData *data, Qt::DropAction action);
    //QStringList mimeTypes();
    //void dragEnterEvent(QDragEnterEvent *event);
    //void dragLeaveEvent(QDragLeaveEvent *event);
    //void dropEvent(QDropEvent *event);
private:
    QSnippet * getItemByName(const QString &name);
    QSnippet * current;
    bool lock;
};

#endif // SIDEBAR_H
