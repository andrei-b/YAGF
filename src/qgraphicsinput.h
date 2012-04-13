/*
    YAGF - cuneiform OCR graphical front-end
    Copyright (C) 2009-2011 Andrei Borovsky <anb@symmetrica.net>

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

#include "sidebar.h"
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QRectF>
#include <QAction>
#include <QList>
#include <QPointer>

typedef QList<QAction *> ActionList;

class QGraphicsPixmapItem;
class QGraphicsRectItem;
class QPixmap;
class QCursor;
class QRectF;
class QCursor;
class QToolBar;
class QActions;

enum SelectStates {
    NoSelect = 0,
    StartSelect,
    Selecting
};


class QGraphicsInput : public QGraphicsScene
{
    Q_OBJECT
public:
    explicit QGraphicsInput(const QRectF &sceneRect, QGraphicsView *view = 0);
    ~QGraphicsInput();
    bool loadImage(const QImage &image, bool clearBlocks = true);
    bool loadNewImage(const QPixmap &image);
    void setView(QGraphicsView *view);
    QImage getActiveBlock();
    QImage getCurrentBlock();
    void setViewScale(qreal scale, qreal angle);
    int blocksCount();
    void deleteBlock(int index);
    void deleteCurrentBlock();
    QImage getBlockByIndex(int index);
    QRectF getBlockRectByIndex(int index);
    void clearBlocks();
    qreal getScale();
    qreal getAngle();
    QPixmap getImage();
    QImage getAdaptedImage();
    QImage * getSmallImage();
    QImage * getImageBy16();
    bool addBlock(const QRectF &rect, bool removeObstacles = true);
    void addBlockColliding(const QRectF &rect);
    void drawLine(int x1, int y1, int x2, int y2);
    void imageOrigin(QPoint &p);
    QPixmap getCurrentImage();
    void setSideBar(SideBar * value);

    void cropImage();
    void cropImage(const QRect &rect);
    void cropWhiteFrame();
    void rotateImage(qreal deg);
    void deskew(QImage *img);
    void splitPage();
    void blockAllText();

    void undo();
    void setMagnifierCursor(QCursor *cursor);
    void addToolBarAction(QAction * action);
    void addToolBarSeparator();
    void setToolBarVisible();
    QGraphicsRectItem *newBlock(const QRectF &rect);

    //setMagnifierCursor(QCursor * cursor = );
protected:
    virtual void mousePressEvent(QGraphicsSceneMouseEvent *event);
    virtual void mouseMoveEvent(QGraphicsSceneMouseEvent *mouseEvent);
    virtual void mouseReleaseEvent(QGraphicsSceneMouseEvent *mouseEvent);
    virtual void wheelEvent(QGraphicsSceneWheelEvent *wheelEvent);
    virtual void keyReleaseEvent(QKeyEvent *keyEvent);
    virtual void keyPressEvent(QKeyEvent *keyEvent);
signals:
    void leftMouseClicked(int x, int y, bool blockSelected);
    void rightMouseClicked(int x, int y, bool inTheBlock);
    void keyPressed(int key);
public slots:
private:
    void leftMouseRelease(qreal x, qreal y);
    void rightMouseRelease(qreal x, qreal y);
    int nearActiveBorder(qreal x, qreal y);
    void clearTransform();
    void addToolBar();
    QImage tryRotate(QImage image, qreal angle);
    QImage extractImage(QGraphicsRectItem *item);
    void deleteBlockRect(QGraphicsRectItem *item);
    QGraphicsView *m_view;
    QGraphicsPixmapItem *m_image;
    QImage m_realImage;
    //QImage old_pixmap;
    QImage pm2;
    QImage pm4;
    QImage pm8;
    QImage pm16;
    QGraphicsRectItem *m_CurrentBlockRect;
    QGraphicsRectItem *m_LastSelected;
    SelectStates selecting;
    QRectF blockRect;
    QRectF selBlockRect;
    bool hasImage;
    qreal  m_scale;
    qreal m_rotate;
    Qt::MouseButton buttonPressed;
    QCursor *magnifierCursor;
    int near_res;
    QPointer<QToolBar> toolbar;
    ActionList actionList;
    SideBar * sideBar;
};

#endif // QGRAPHICSINPUT_H
