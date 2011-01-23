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

#include "qgraphicsinput.h"
#include <QPixmap>
#include <QGraphicsPixmapItem>
#include <QMessageBox>
#include <QGraphicsSceneMouseEvent>
#include <QKeyEvent>

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
    m_rotate = 0;
    buttonPressed = Qt::NoButton;
    near_res = 0;
    magnifierCursor = new QCursor(Qt::SizeAllCursor);
}

bool QGraphicsInput::loadImage(const QPixmap &image, bool clearBlocks)
{
    if (clearBlocks || (!hasImage)) {
        real_rotate = 0;
        real_scale = 1;
        this->clear();
        this->items().clear();
        m_LastSelected = 0;
        m_CurrentBlockRect = 0;
    }
    if ((!clearBlocks) && hasImage) {
        this->removeItem(m_image);
        real_scale = 1;
    }
    old_pixmap = image;
    pm2 = image.scaledToWidth(image.width()/2);
    pm4 = pm2.scaledToWidth(pm2.width()/2);
    pm8 = pm4.scaledToWidth(pm4.width()/2);
    pm16 = pm8.scaledToWidth(pm8.width()/2);
    this->setSceneRect(image.rect());
    m_image = this->addPixmap(image);
    m_realImage = this->addPixmap(image);
    m_realImage->setData(1, "image");
    m_realImage->hide();
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
            if((near_res = nearActiveBorder(event->scenePos().x(), event->scenePos().y())) != 0)
            {
                m_CurrentBlockRect = m_LastSelected;
                selecting = Selecting;
                blockRect = m_CurrentBlockRect->rect();
            }  else {
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


QGraphicsRectItem * QGraphicsInput::newBlock(const QRectF &rect)
{
    QPen p(Qt::SolidLine);
    QBrush b(Qt::SolidPattern);
    b.setColor(QColor(0,0,127,127));
    p.setWidth(2);
    p.setColor(QColor(0,0, 255));
    QGraphicsRectItem * res;
    res = this->addRect(rect, p, b);
    res->setAcceptHoverEvents(true);
    res->setZValue(1);
    res->setData(1, "block");
    res->setData(2, "no");
    return res;
}

bool QGraphicsInput::addBlock(const QRectF &rect, bool removeObstacles)
{
    QGraphicsRectItem * block = newBlock(rect);
    if (!removeObstacles) {
        if (block->collidingItems().size() > 0) {
            deleteBlockRect(block);
            return false;
        }
    } else {
        for (int i = block->collidingItems().size() - 1; i >= 0; i--) {
           if (block->collidingItems().at(i)->data(1) == "block")
             deleteBlockRect((QGraphicsRectItem *) block->collidingItems().at(i));
        }
    }
    m_CurrentBlockRect = block;
    return true;
}

void QGraphicsInput::mouseMoveEvent(QGraphicsSceneMouseEvent *mouseEvent)
{
    if (selecting == StartSelect) {
        selecting = Selecting;
        m_CurrentBlockRect = newBlock(blockRect);
    }

    if ((mouseEvent->modifiers() & Qt::ControlModifier) == 0)
    if (mouseEvent->buttons() == Qt::NoButton) {
        near_res = nearActiveBorder(mouseEvent->scenePos().x(), mouseEvent->scenePos().y());
            switch (near_res) {
            case 0:
                m_view->setCursor(Qt::ArrowCursor);
                break;
            case 1:
                m_view->setCursor(Qt::SplitHCursor);
                break;
            case 2:
                m_view->setCursor(Qt::SplitVCursor);
                break;
            case 3:
                m_view->setCursor(Qt::SplitHCursor);
                break;
            case 4:
                m_view->setCursor(Qt::SplitVCursor);
                break;
            default:
                break;
            }

    }
    QRectF newRect;
    if (near_res && (mouseEvent->buttons()&Qt::LeftButton)) {
        QRectF newRect = m_LastSelected->mapRectToScene(m_LastSelected->rect());
        switch (near_res) {
        case 1:
            newRect.setLeft(mouseEvent->lastScenePos().x());
            break;
        case 2:
            newRect.setTop(mouseEvent->lastScenePos().y());
            break;
        case 3:
            newRect.setRight(mouseEvent->lastScenePos().x());
            break;
        case 4:
            newRect.setBottom(mouseEvent->lastScenePos().y());
            break;
        default:
            break;
        }
        m_CurrentBlockRect = m_LastSelected;
        m_CurrentBlockRect->setRect(m_LastSelected->mapRectFromScene(newRect));
        for (int i = 0; i < m_CurrentBlockRect->collidingItems().size(); ++i)
            if (m_CurrentBlockRect->collidingItems().at(i)->data(1) == "block") {
                 m_CurrentBlockRect->setRect(m_LastSelected->mapRectFromScene(selBlockRect));
                 return;
             }
        selBlockRect = newRect;
        return;
    }
    if (selecting == Selecting)
    {
        newRect = blockRect;
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
                selBlockRect = m_LastSelected->rect();
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
    emit leftMouseClicked(m_view->mapFromScene(x, y).x(), m_view->mapFromScene(x, y).y(), m_CurrentBlockRect != 0);
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
    emit rightMouseClicked(m_view->mapFromScene(x, y).x(), m_view->mapFromScene(x, y).y(), m_CurrentBlockRect != 0);
}


int QGraphicsInput::nearActiveBorder(qreal x, qreal y)
{
    if (m_LastSelected == 0)
        return 0;
        x = m_LastSelected->mapFromScene(x, y).x();
        y = m_LastSelected->mapFromScene(x, y).y();
        qreal xcenter = m_LastSelected->rect().center().x();
        qreal ycenter = m_LastSelected->rect().center().y();
        qreal xcd = abs(m_LastSelected->rect().right() - xcenter) + 8;
        qreal ycd = abs(m_LastSelected->rect().bottom() - ycenter) + 8;
        if ((abs(x - m_LastSelected->rect().left()) <= 4)) {
            if (abs(y - ycenter) < ycd)
                return 1;
            else
                return 0;
        }
        if ((abs(m_LastSelected->rect().top() - y) <= 4)) {
            if (abs(x - xcenter) < xcd)
                return 2;
            else
                return 0;
        }
        if ((abs(x - m_LastSelected->rect().right()) <= 4)) {
            if (abs(y - ycenter) < ycd)
                return 3;
            else
                return 0;
        }
        if ((abs(m_LastSelected->rect().bottom() - y) <= 4)) {
            if (abs(x - xcenter) < xcd)
                return 4;
            else
                return 0;
        }
        return 0;
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
    QRectF rect = item->mapRectToScene(item->rect());
    return m_realImage->pixmap().copy(rect.left()/real_scale, rect.top()/real_scale,(rect.right()-rect.left())/real_scale,(rect.bottom()-rect.top())/real_scale);
}

void QGraphicsInput::setViewScale(qreal scale, qreal angle)
{
    if (!hasImage) return;
    if ((scale == 0)||((real_scale*scale) < 0.0625)||(real_scale*scale) > 1)
        return;
    m_scale = scale;
//    m_view->scale(scale,  scale);
    this->removeItem(m_image);
    for (int i = 0; i < this->items().size(); i++)
        if (items().at(i)->data(1) != "image")
            items().at(i)->scale(scale, scale); // Silly as this line seems it is the only way to scale rectangles correctly.
    real_scale *= scale;

    if (real_scale == 1)
            m_image = this->addPixmap(old_pixmap);
    else
    if (real_scale == 0.5)
       m_image = this->addPixmap(pm2);
    else
    if (real_scale == 0.25)
       m_image = this->addPixmap(pm4);
    else
    if (real_scale == 0.125)
      m_image = this->addPixmap(pm8);
    else
    if (real_scale == 0.0625)
        m_image = this->addPixmap(pm16);
    m_image->setFocus();
    m_image->setAcceptedMouseButtons(Qt::LeftButton|Qt::RightButton|Qt::MidButton);
    m_image->setAcceptHoverEvents(true);
    m_image->setData(1, "image");
    qreal x = width()/2;
    qreal y = height()/2;
    real_rotate += angle;
    m_image->setPixmap(m_image->pixmap().transformed(QTransform().translate(-x, -y).rotate(real_rotate).translate(x, y)));
    m_realImage->setPixmap(m_realImage->pixmap().transformed(QTransform().translate(-x, -y).rotate(angle).translate(x, y)));
    m_rotate = angle;
    m_image->show();
    m_view->centerOn(0,0);
}

void QGraphicsInput::rotateImage(qreal angle, qreal x, qreal y)
{
    m_rotate = angle;
    //real_rotate = real_rotate % 360;
     setViewScale(1, angle);
    //QPixmap pm = m_image->pixmap().transformed(QTransform().translate(-x, -y).rotate(angle).translate(x, y));
   // loadImage(pm, false);
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

void QGraphicsInput::deleteCurrentBlock()
{
    if (m_CurrentBlockRect != 0)
        deleteBlockRect(m_CurrentBlockRect);
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
    return QPixmap(0,0);
}

QRectF QGraphicsInput::getBlockRectByIndex(int index)
{
    int count = 0;
    for (int i = 0; i < items().count(); i++) {
        if (items().at(i)->data(1) == "block") {
            if (index == count) {
                QRectF rect = items().at(i)->mapRectToScene(((QGraphicsRectItem *) items().at(i))->rect());
                return QRectF(rect.left()/real_scale, rect.top()/real_scale,(rect.right()-rect.left())/real_scale,(rect.bottom()-rect.top())/real_scale);
           }
           count++;
        }
    }
    return QRectF(0,0,0,0);
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
    return hasImage ? m_realImage->pixmap() : 0;
}

QPixmap QGraphicsInput::getAdaptedImage()
{
    if (!hasImage)
        return QPixmap(0,0);
    if (m_realImage->pixmap().width() > 8000)
            return this->pm8;
    if (m_realImage->pixmap().width() > 4000)
            return this->pm4;
    if (m_realImage->pixmap().width() > 2000)
            return this->pm2;
    return m_realImage->pixmap();
}


void QGraphicsInput::cropImage()
{
    if (!hasImage)
        return;
    if (m_LastSelected) {
        //QPixmap pm = extractPixmap(m_LastSelected);
        loadImage(extractPixmap(m_LastSelected));
        clearTransform();
    }
}

void QGraphicsInput::setMagnifierCursor(QCursor *cursor)
{
    delete magnifierCursor;
    magnifierCursor = new QCursor(cursor->pixmap());
}

void QGraphicsInput::undo()
{
    if (hasImage)
        loadImage(old_pixmap);
        clearTransform();
}

void QGraphicsInput::wheelEvent(QGraphicsSceneWheelEvent *wheelEvent)
{
    if (wheelEvent->modifiers() == Qt::ControlModifier) {
            int delta = wheelEvent->delta();
            qreal coeff = delta < 0 ? 1/(1-delta/(360.)) : 1 + delta/(240.);
            if (coeff >= 1)
                coeff = 2;
            else
                coeff = 0.5;
            this->setViewScale(coeff, 0);
            wheelEvent->accept();
            m_view->setCursor(*magnifierCursor);
     } else
         QGraphicsScene::wheelEvent(wheelEvent);
}

void QGraphicsInput::keyReleaseEvent(QKeyEvent *keyEvent)
{
    if (keyEvent->key() == Qt::Key_Control)
        m_view->setCursor(Qt::ArrowCursor);
    if (keyEvent->modifiers() & Qt::ControlModifier) {
        if ((keyEvent->key() == Qt::Key_Plus)||(keyEvent->key() == Qt::Key_Equal)) {
            this->setViewScale(2, 0);
            return;
        }
        if ((keyEvent->key() == Qt::Key_Minus)||(keyEvent->key() == Qt::Key_Underscore)) {
            this->setViewScale(0.5, 0);
            return;
        }

    }
    if (keyEvent->key() > Qt::Key_F1) {
        emit keyPressed((int)keyEvent->key());
    }
}

void QGraphicsInput::clearTransform()
{
    if (m_view) {
        QTransform tr = m_view->transform();
        tr.reset();
        m_view->setTransform(tr);
    }
}

bool QGraphicsInput::loadNewImage(const QPixmap &image)
{
    clearTransform();
    return loadImage(image);
}

void QGraphicsInput::keyPressEvent ( QKeyEvent * keyEvent )
{
     if (keyEvent->key() == Qt::Key_Control)
         m_view->setCursor(*magnifierCursor);
}
