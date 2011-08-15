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

#include "qxtgraphicsproxywidget.h"
#include "qxtgraphicsview.h"
#include <QVariant>
#include <QGraphicsItem>
#include <QPointF>

QXtGraphicsProxyWidget::QXtGraphicsProxyWidget(QGraphicsItem * parent, Qt::WindowFlags wFlags) : QGraphicsProxyWidget(parent, wFlags)
{
    setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
}

void QXtGraphicsProxyWidget::setView(QXtGraphicsView * view)
{
    mview = view;
    QPointF p = mview->mapToScene(0,0);
    setPos(p.x(), p.y());
    connect(mview, SIGNAL(scrolled()), this, SLOT(viewScrolled()));
}

void QXtGraphicsProxyWidget::viewScrolled()
{
    QPointF p = mview->mapToScene(0,0);
    setPos(p.x(), p.y());
}

QVariant QXtGraphicsProxyWidget::itemChange(GraphicsItemChange change, const QVariant & value)
{
    QVariant v;
    switch (change) {
    case QGraphicsItem::ItemScaleChange:
        v = QVariant(1.0);
        return v;
    case QGraphicsItem::ItemRotationChange:
        v = QVariant(0.0);
        return v;
    case QGraphicsItem::ItemTransformChange:
            v = QVariant(matrix());
            return v;
    default:
            return value;
    }
}
