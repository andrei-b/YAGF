#include "qgraphicsinput.h"
#include <QPixmap>
#include <QGraphicsPixmapItem>
#include <QMessageBox>
#include <QGraphicsSceneMouseEvent>

QGraphicsInput::QGraphicsInput(const QRectF & sceneRect, QGraphicsView * view) :
    QGraphicsScene(sceneRect)
{
    setView(view);
    m_image = 0;
    selecting  = false;
    hasImage = false;
}

bool QGraphicsInput::loadImage(const QPixmap &image)
{
    this->clear();
    this->items().clear();
    this->setSceneRect(image.rect());
    m_image = this->addPixmap(image);
    this->setFocus();
    m_image->setFocus();
    m_image->setAcceptedMouseButtons(Qt::LeftButton|Qt::RightButton|Qt::MidButton);
    m_image->setAcceptHoverEvents(true);
    m_image->setData(1, "image");
    if (m_view) {
        m_view->centerOn(0, 0);
        m_view->show();
        hasImage = true;
        return true;
    } else
        return false;
}

void QGraphicsInput::setView(QGraphicsView *view) {
    m_view = view;
    if (m_view)
        m_view->setScene(this);

}

void QGraphicsInput::mousePressEvent ( QGraphicsSceneMouseEvent * event )
{
   // QMessageBox::critical(0, "MOUS111", "MOUSE");
    if (!hasImage)
        return;
    if (event->buttons() == Qt::LeftButton) {
        if (!selecting) {
            selecting = true;
            blockRect.setLeft(event->lastScenePos().x());
            blockRect.setTop(event->lastScenePos().y());
            blockRect.setWidth(10);
            blockRect.setHeight(10);
            QPen p(Qt::SolidLine);
            QBrush b(Qt::SolidPattern);
            b.setColor(QColor(0,0,127,127));
            p.setWidth(2);
            p.setColor(QColor(0,0, 255));
            m_CurrentBlockRect = this->addRect(blockRect, p, b);
            m_CurrentBlockRect->setZValue(1);
            m_CurrentBlockRect->setData(1, "block");
        } else {
        //TODO!!!
        }
    }
}

void QGraphicsInput::mouseReleaseEvent(QGraphicsSceneMouseEvent *mouseEvent)
{
    selecting = false;
}


void QGraphicsInput::mouseMoveEvent(QGraphicsSceneMouseEvent *mouseEvent)
{
    if (selecting) {
        QRectF newRect = blockRect;
        if (newRect.left() < mouseEvent->lastScenePos().x())
            newRect.setRight(mouseEvent->lastScenePos().x());
        else
            newRect.setLeft(mouseEvent->lastScenePos().x());
        if (newRect.top() < mouseEvent->lastScenePos().y())
            newRect.setBottom(mouseEvent->lastScenePos().y());
        else
            newRect.setTop(mouseEvent->lastScenePos().y());
        m_CurrentBlockRect->setRect(newRect);
        for (int i = 0; i < m_CurrentBlockRect->collidingItems().size(); ++i)
            if (m_CurrentBlockRect->collidingItems().at(i)->data(1) == "block") {
                 m_CurrentBlockRect->setRect(blockRect);
                 return;
             }
        blockRect = newRect;
        return;
    }
}
