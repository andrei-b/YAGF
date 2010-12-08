#ifndef QGRAPHICSINPUT_H
#define QGRAPHICSINPUT_H

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QRectF>

class QGraphicsPixmapItem;
class QGraphicsRectItem;
class QPixmap;


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
    bool loadImage(const QPixmap &image);
    void setView(QGraphicsView * view);
protected:
    virtual void mousePressEvent ( QGraphicsSceneMouseEvent * event );
    virtual void mouseMoveEvent ( QGraphicsSceneMouseEvent * mouseEvent );
    virtual void mouseReleaseEvent ( QGraphicsSceneMouseEvent * mouseEvent);
signals:
    void leftMouseClicked(int x, int y, bool blockSelected);
    void rightMouseClicked(int x, int y, bool inTheBlock);
public slots:
private:
    void leftMouseRelease(qreal x, qreal y);
    QGraphicsView * m_view;
    QGraphicsPixmapItem * m_image;
    QGraphicsRectItem * m_CurrentBlockRect;
    QGraphicsRectItem * m_LastSelected;
    SelectStates selecting;
    QRectF blockRect;
    bool hasImage;
};

#endif // QGRAPHICSINPUT_H
