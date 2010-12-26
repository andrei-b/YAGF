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

#ifndef QGRAPHICSINPUT_H
#define QGRAPHICSINPUT_H

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QRectF>

class QGraphicsPixmapItem;
class QGraphicsRectItem;
class QPixmap;
class QCursor;
class QRectF;
class QCursor;

enum SelectStates {
    NoSelect = 0,
    StartSelect,
    Selecting
};


class QGraphicsInput : public QGraphicsScene
{
    Q_OBJECT
public:
    explicit QGraphicsInput(const QRectF & sceneRect, QGraphicsView * view = 0);
    bool loadImage(const QPixmap &image, bool clearBlocks = true);
    bool loadNewImage(const QPixmap &image);
    void setView(QGraphicsView * view);
    QPixmap getActiveBlock();
    QPixmap getCurrentBlock();
    void setViewScale(qreal scale);
    void rotateImage(qreal angle, qreal x, qreal y);
    int blocksCount();
    void deleteBlock(int index);
    QPixmap getBlockByIndex(int index);
    void clearBlocks();
    qreal getRealScale();
    qreal getRealAngle();
    QPixmap getImage();
    bool addBlock(const QRectF &rect, bool removeObstacles = true);
    void cropImage();
    void undo();
    void setMagnifierCursor(QCursor * cursor);
    //setMagnifierCursor(QCursor * cursor = );
protected:
    virtual void mousePressEvent ( QGraphicsSceneMouseEvent * event );
    virtual void mouseMoveEvent ( QGraphicsSceneMouseEvent * mouseEvent );
    virtual void mouseReleaseEvent ( QGraphicsSceneMouseEvent * mouseEvent);
    virtual void wheelEvent ( QGraphicsSceneWheelEvent * wheelEvent );
    virtual void keyReleaseEvent ( QKeyEvent * keyEvent );
    virtual void keyPressEvent ( QKeyEvent * keyEvent );
signals:
    void leftMouseClicked(int x, int y, bool blockSelected);
    void rightMouseClicked(int x, int y, bool inTheBlock);
    void keyPressed(int key);
public slots:
private:
    void leftMouseRelease(qreal x, qreal y);
    void rightMouseRelease(qreal x, qreal y);
    bool nearActiveBorder(qreal x, qreal y);
    void clearTransform();
    QPixmap extractPixmap(QGraphicsRectItem * item);
    void deleteBlockRect(QGraphicsRectItem * item);
    QGraphicsRectItem * newBlock(const QRectF &rect);
    QGraphicsView * m_view;
    QGraphicsPixmapItem * m_image;
    QPixmap old_pixmap;
    QGraphicsRectItem * m_CurrentBlockRect;
    QGraphicsRectItem * m_LastSelected;
    SelectStates selecting;
    QRectF blockRect;
    bool hasImage;
    qreal  m_scale;
    qreal real_scale;
    qreal m_rotate;
    qreal real_rotate;
    Qt::MouseButton buttonPressed;
    QCursor * magnifierCursor;
};

#endif // QGRAPHICSINPUT_H
