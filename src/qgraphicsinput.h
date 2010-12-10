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
    bool loadImage(const QPixmap &image, bool clearBlocks = true);
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
    void cropImage();
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
    void rightMouseRelease(qreal x, qreal y);
    bool nearActiveBorder(qreal x, qreal y);
    QPixmap extractPixmap(QGraphicsRectItem * item);
    void deleteBlockRect(QGraphicsRectItem * item);
    QGraphicsView * m_view;
    QGraphicsPixmapItem * m_image;
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
};

#endif // QGRAPHICSINPUT_H
