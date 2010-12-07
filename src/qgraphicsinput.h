#ifndef QGRAPHICSINPUT_H
#define QGRAPHICSINPUT_H

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QRectF>

class QGraphicsPixmapItem;
class QGraphicsRectItem;
class QPixmap;


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

public slots:
private:
    QGraphicsView * m_view;
    QGraphicsPixmapItem * m_image;
    QGraphicsRectItem * m_CurrentBlockRect;
    bool selecting;
    QRectF blockRect;
    bool hasImage;
};

#endif // QGRAPHICSINPUT_H
