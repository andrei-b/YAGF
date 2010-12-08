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
    selecting  = NoSelect;
    hasImage = false;
    m_LastSelected = 0;
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
        if (selecting == NoSelect) {
            selecting = StartSelect;
            blockRect.setLeft(event->lastScenePos().x());
            blockRect.setTop(event->lastScenePos().y());
            blockRect.setWidth(10);
            blockRect.setHeight(10);
        } else {
        //TODO!!!
        }
    }
}

void QGraphicsInput::mouseReleaseEvent(QGraphicsSceneMouseEvent *mouseEvent)
{
    if (selecting == Selecting) {
        selecting = NoSelect;
        if ((blockRect.width() < 12)||(blockRect.height() < 12)) {
            this->removeItem(m_CurrentBlockRect);
            //clik!!!
            leftMouseRelease(mouseEvent->scenePos().x(), mouseEvent->scenePos().y());
        }
        m_CurrentBlockRect = 0;
    }
    if (selecting == StartSelect) {
        selecting = NoSelect;
        m_CurrentBlockRect = 0;
        leftMouseRelease(mouseEvent->scenePos().x(), mouseEvent->scenePos().y());
    }

}


void QGraphicsInput::mouseMoveEvent(QGraphicsSceneMouseEvent *mouseEvent)
{
    if (selecting == StartSelect) {
        selecting = Selecting;
        QPen p(Qt::SolidLine);
        QBrush b(Qt::SolidPattern);
        b.setColor(QColor(0,0,127,127));
        p.setWidth(2);
        p.setColor(QColor(0,0, 255));
        m_CurrentBlockRect = this->addRect(blockRect, p, b);
        m_CurrentBlockRect->setZValue(1);
        m_CurrentBlockRect->setData(1, "block");
        m_CurrentBlockRect->setData(2, "no");
    }
    if (selecting == Selecting)
    {

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

void QGraphicsInput::leftMouseRelease(qreal x, qreal y)
{
    QGraphicsItem * it = this->itemAt(x, y);
    if (it) {
        if (it->data(1).toString() == "block") {
            QGraphicsRectItem * r = (QGraphicsRectItem*) it;
            QPen p(Qt::SolidLine);
            QBrush b(Qt::SolidPattern);
            b.setColor(QColor(0,0,127,127));
            p.setColor(QColor(0, 0, 255));
            p.setWidth(2);
            if (r->data(2).toString() == "no") {
                //select block!!!!
                if (m_LastSelected) {
                    m_LastSelected->setPen(p);
                    m_LastSelected->setBrush(b);
                    m_LastSelected->setData(2, "no");
                }
                b.setColor(QColor(127,0,0,127));
                p.setColor(QColor(255,0,0));
                r->setData(2, "yes");
                m_LastSelected = r;
            } else {
                m_LastSelected = 0;
                r->setData(2, "no");
            }
            r->setPen(p);
            r->setBrush(b);
        }
    }

}
