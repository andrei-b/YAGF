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
    m_scale = 1;
    real_scale = 1;
    real_rotate = 0;
    buttonPressed = Qt::NoButton;
}

bool QGraphicsInput::loadImage(const QPixmap &image, bool clearBlocks)
{
    if (clearBlocks || (!hasImage)) {
        this->clear();
        this->items().clear();
        m_LastSelected = 0;
        m_CurrentBlockRect = 0;
    }
    if ((!clearBlocks) && hasImage)
        this->removeItem(m_image);
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
        buttonPressed = Qt::LeftButton;
        if (selecting == NoSelect) {
            if (this->nearActiveBorder(event->scenePos().x(), event->scenePos().y())) {
                m_CurrentBlockRect = m_LastSelected;
                selecting = Selecting;
                blockRect = m_CurrentBlockRect->rect();
            } else {
                selecting = StartSelect;
                blockRect.setLeft(event->lastScenePos().x());
                blockRect.setTop(event->lastScenePos().y());
                blockRect.setWidth(10);
                blockRect.setHeight(10);
            }
        } else {
        //TODO!!!
        }
    } else
        buttonPressed = Qt::RightButton;
}

void QGraphicsInput::deleteBlockRect(QGraphicsRectItem *item)
{
    if (item == 0)
        return;
    if (item == m_CurrentBlockRect)
        m_CurrentBlockRect = 0;
    if (item == m_LastSelected)
        m_LastSelected = 0;
    removeItem(item);
}

void QGraphicsInput::mouseReleaseEvent(QGraphicsSceneMouseEvent *mouseEvent)
{
    if (buttonPressed == Qt::LeftButton) {
        if (selecting == Selecting) {
            selecting = NoSelect;
            if ((blockRect.width() < 12)||(blockRect.height() < 12)) {
                if (m_CurrentBlockRect == m_LastSelected)
                    m_LastSelected = 0;
                    deleteBlockRect(m_CurrentBlockRect);
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
    if (buttonPressed == Qt::RightButton) {
        this->rightMouseRelease(mouseEvent->scenePos().x(), mouseEvent->scenePos().y());
    }
    buttonPressed = Qt::NoButton;
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
        m_CurrentBlockRect->setAcceptHoverEvents(true);
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
    if (mouseEvent->buttons() == Qt::NoButton) {
        if (this->nearActiveBorder(mouseEvent->scenePos().x(), mouseEvent->scenePos().y()))
            m_view->setCursor(Qt::SizeAllCursor);
        else
            m_view->setCursor(Qt::ArrowCursor);
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
   //         m_CurrentBlockRect = r;
        }
    } else
        m_CurrentBlockRect = 0;
    emit leftMouseClicked(x, y, m_CurrentBlockRect != 0);
}


void QGraphicsInput::rightMouseRelease(qreal x, qreal y)
{
    QGraphicsItem * it = this->itemAt(x, y);
    if (it) {
        if (it->data(1).toString() == "block") {
            m_CurrentBlockRect = (QGraphicsRectItem*) it;
        }
    } else
        m_CurrentBlockRect = 0;
    emit rightMouseClicked(x, y, m_CurrentBlockRect != 0);
}


bool QGraphicsInput::nearActiveBorder(qreal x, qreal y)
{
    if (m_LastSelected) {
        qreal xcenter = m_LastSelected->rect().center().x();
        qreal ycenter = m_LastSelected->rect().center().y();
        qreal xd = abs(x-xcenter);
        qreal yd = abs(y-ycenter);
        if ((abs(abs(m_LastSelected->rect().left() - xcenter) - xd) <= 4) &&
                    (abs(abs(m_LastSelected->rect().top() - ycenter) - yd) <= abs(m_LastSelected->rect().top() - ycenter)))
            return true;
        if ((abs(abs(m_LastSelected->rect().left() - xcenter) - xd) <= abs(m_LastSelected->rect().left() - xcenter)) &&
                    (abs(abs(m_LastSelected->rect().top() - ycenter) - yd) <= 4))
            return true;

    }
    return false;
}

QPixmap QGraphicsInput::getActiveBlock()
{
    return extractPixmap(m_LastSelected);
}

QPixmap QGraphicsInput::getCurrentBlock()
{
    return extractPixmap(m_CurrentBlockRect);
}


QPixmap QGraphicsInput::extractPixmap(QGraphicsRectItem *item)
{
    if ((item == 0) || (!hasImage)) {
        return QPixmap(0,0);
    }
//    QMessageBox::critical(0,QString::number(m_LastSelected->rect().right()), QString::number(m_LastSelected->rect().bottom()));
    int imgl, imgt, imgr, imgb;
 //   imgl = item->mapToItem(m_image, item->rect().left(), item->rect().top()).x();
 //   imgt = item->mapToItem(m_image, item->rect().left(), item->rect().top()).y();
 //   imgr = item->mapToItem(m_image, item->rect().right(), item->rect().bottom()).x();
 //   imgb = item->mapToItem(m_image, item->rect().right(), item->rect().bottom()).y();
       imgl = item->rect().left();
       imgt = item->rect().top();
       imgr = item->rect().right();
       imgb = item->rect().bottom();
    return m_image->pixmap().copy(imgl, imgt,imgr-imgl,imgb-imgt);
}

void QGraphicsInput::setViewScale(qreal scale)
{
    if (scale == 0)
        return;
    m_scale = scale/m_scale;
    real_scale *= m_scale;
    this->m_view->scale(m_scale,  m_scale);

}

void QGraphicsInput::rotateImage(qreal angle, qreal x, qreal y)
{
    m_rotate = angle;
    real_rotate += angle;
    //real_rotate = real_rotate % 360;

    QPixmap pm = m_image->pixmap().transformed(QTransform().translate(-x, -y).rotate(angle).translate(x, y));
    loadImage(pm, false);
    //m_image->setPixmap(pm);
}

int QGraphicsInput::blocksCount()
{
    int res = 0;
    for (int i = 0; i < items().count(); i++)
        if (items().at(i)->data(1) == "block")
            res++;
    return res;
}

void QGraphicsInput::deleteBlock(int index)
{
    int count = 0;
    for (int i = 0; i < items().count(); i++) {
        if (items().at(i)->data(1) == "block") {
            if (index == count) {
                deleteBlockRect((QGraphicsRectItem *)items().at(i));
                return;
           }
           count++;
        }
    }
}

QPixmap QGraphicsInput::getBlockByIndex(int index)
{
    int count = 0;
    for (int i = 0; i < items().count(); i++) {
        if (items().at(i)->data(1) == "block") {
            if (index == count) {
                return extractPixmap((QGraphicsRectItem *)items().at(i));
           }
           count++;
        }
    }
    return 0;
}

void QGraphicsInput::clearBlocks()
{
    for (int i = items().count() - 1; i >= 0; i--) {
        if (items().at(i)->data(1) == "block") {
                deleteBlockRect((QGraphicsRectItem *)items().at(i));
           }
    }
}

qreal QGraphicsInput::getRealScale()
{
    return real_scale;
}

qreal QGraphicsInput::getRealAngle()
{
    return real_rotate;
}

QPixmap QGraphicsInput::getImage()
{
    return hasImage ? m_image->pixmap() : 0;
}

void QGraphicsInput::cropImage()
{
    if (!hasImage)
        return;
    if (m_LastSelected) {
        //QPixmap pm = extractPixmap(m_LastSelected);
        loadImage(extractPixmap(m_LastSelected));
    }
}
