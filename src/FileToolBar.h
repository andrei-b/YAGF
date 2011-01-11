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

#ifndef FILETOOLBAR_H
#define FILETOOLBAR_H

#include <QToolBar>
#include <QString>
#include <QStringList>
#include <QRectF>
#include <QMap>

typedef QMap<QString, QString> StringMap;
typedef QMap<QString, int> IntMap;
typedef QMap<QString, float> FloatMap;
typedef QMap<QString, QRectF> RectMap;

class QPixmap;
class QPushButton;
class QRect;
class QDragEnterEvent;
class QDropEvent;
class QDragLeaveEvent;

//typedef QList<QRect> RectList;
//typedef class QMultiMap<QString, QRect> BlocksMap;


class FileToolBar : public QToolBar
{
Q_OBJECT
public:
    FileToolBar(QWidget * parent);
    void addFile(const QPixmap & pixmap, const QString & name);
    void addFile(const QString & name);
    QStringList getFileNames();
    void setRotation(int r);
    void setRotation(const QString &name, int r);
    void setBlock(const QRectF &block);
    void setBlock(const QString &name, const QRectF &block);
    int getBlocksCount();
    void removeBlock(const QRectF &block);
    void setScale(float s);
    float getScale();
    float getScale(const QString &name);
    int getRotation();
    void addBlock(const QRect &rect);
   // RectList getBlocks();
    void clearBlocks();
    QRectF getBlock(int index);
    int getRotation(const QString &name);
    bool fileLoaded(const QString &name);
    void select(const QString &name);
    void selectFirstFile();
signals:
    void fileSelected(const QString & name);
protected:
    void dragEnterEvent (QDragEnterEvent * event);
    void dragLeaveEvent ( QDragLeaveEvent * event );
    void dropEvent (QDropEvent * event);
private:
    bool buttonsAdded;
    StringMap * filesMap;
    IntMap * rotMap;
    FloatMap * scaleMap;
    RectMap * blocksMap;
    QPushButton * saveButton;
    QPushButton * clearButton;
    QPushButton * removeButton;
    QString currentImage;
private slots:
    void saveAll();
    void selected();
    void clearAll();
    void remove();
};


#endif // FILETOOLBAR_H
