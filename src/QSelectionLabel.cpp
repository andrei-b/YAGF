/*
    YAGF - cuneiform OCR graphical front-end 
    Copyright (C) 2009 Andrei Borovsky <anb@symmetrica.net>

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

#include "QSelectionLabel.h"
#include <QPixmap>
#include <QRect>
#include <QMouseEvent>
#include <QPainter>
#include <QBrush>

QSelectionLabel::QSelectionLabel(QWidget *parent, Qt::WindowFlags f):QLabel(parent, f)
{
	x0 = y0 = x1 = y1 = 0;
	oldCopy = 0;
	selecting = started = selected = false;
	this->setAlignment(Qt::AlignLeft|Qt::AlignTop);
        setFocusPolicy(Qt::ClickFocus);
        setPixmap(QPixmap(0, 0));
}

bool QSelectionLabel::isSelectionEmpty()
{
	return ((x0==x1)&&(y0==y1));
}

void QSelectionLabel::setSelectionMode(bool mode)
{
	selecting = mode;
}

bool QSelectionLabel::getSelectionMode()
{
	return selecting;
}

void QSelectionLabel::resetSelection(bool redraw)
{
     if (!pixmap()->isNull()) {
		if (redraw)
			restoreRect();
	if (oldCopy) {
		delete oldCopy;
		oldCopy = 0;
	}	

		x0 = 0;
		y0 = 0;
		x1 = pixmap()->width();
		y1 = pixmap()->height();
	}
	else {
		x0 = y0 = x1 = y1 = 0;
	}
	started = false;
	selected = false;
}

QRect QSelectionLabel::getSelectedRect()
{
	int xt, yt, w, h;
	if ((x1-x0) >= 0) {
		xt = x0;
		w = x1-x0;
	} else {
		xt = x1;
		w = x0-x1;
	}
	if ((y1-y0) >= 0) {
		yt = y0;
		h = y1-y0;
	} else {
		yt = y1;
		h = y0-y1;
	}
	return QRect(xt, yt, w, h);
}

void QSelectionLabel::mousePressEvent(QMouseEvent *ev)
{
        if (pixmap()->isNull())
            return;
        if (ev->button() == Qt::LeftButton) {
		if (started||selected) 
			resetSelection(true);
		if (pixmap()->isNull())
			return;
		started = true;
		selected = false;
		x0 = ev->x();
		y0 = ev->y();
		x1 = x0;
		y1 = y0;
		drawRect();
	}
}

void QSelectionLabel::mouseMoveEvent(QMouseEvent *ev)
{
	if (started) {
		restoreRect();
		if ((ev->x() >= 0) && (ev->x() <= pixmap()->width()))
			x1 = ev->x();
		if ((ev->y() >= 0) && (ev->y() <= pixmap()->height()))
			y1 = ev->y();
		drawRect();
	}
}

void QSelectionLabel::mouseReleaseEvent(QMouseEvent *ev)
{
        if (pixmap()->isNull())
        return;

        if ((ev->button() == Qt::LeftButton)  && started) {
		selected = true;
		started = false;
		restoreRect();
		if ((ev->x() >= 0) && (ev->x() <= pixmap()->width()))
			x1 = ev->x();
		if ((ev->y() >= 0) && (ev->y() <= pixmap()->height()))
			y1 = ev->y();
		drawRect();
		if ((abs(x1-x0) < 3) || (abs(y1-y0) < 3)) {
			resetSelection(true);
		}
	}
}

void QSelectionLabel::drawRect() 
{
	if (oldCopy) {
		delete oldCopy;
		oldCopy = 0;
	}
	int xt, yt, w, h;
	if ((x1-x0) >= 0) {
		xt = x0;
		w = x1-x0;
	} else {
		xt = x1;
		w = x0-x1;
	}
	if ((y1-y0) >= 0) {
		yt = y0;
		h = y1-y0;
	} else {
		yt = y1;
		h = y0-y1;
	}
	oldCopy = new QPixmap((pixmap()->copy(xt, yt, w, h)));
	QPainter painter;
	painter.begin((QPaintDevice *) pixmap());
	painter.setPen(Qt::NoPen);
	painter.setBrush(QBrush(QColor(0, 0, 255), Qt::Dense5Pattern));
	if ((x1-x0) >= 0) {
		xt = x0;
		w = x1-x0;
        }
        else
        {
		xt = x1;
		w = x0-x1;
	}
	if ((y1-y0) >= 0) {
		yt = y0;
		h = y1-y0;
	} else {
		yt = y1;
		h = y0-y1;
	}
	painter.drawRect(xt, yt, w, h);
	painter.end();
	update();
		
}

void QSelectionLabel::restoreRect()
{
	QPainter painter;
	painter.begin((QPaintDevice *) pixmap());
	if (oldCopy)
		if (!oldCopy->isNull()) {
			int xt, yt, w, h;
			if ((x1-x0) >= 0) {
				xt = x0;
				w = x1-x0;
			} else {
				xt = x1;
				w = x0-x1;
			}
			if ((y1-y0) >= 0) {
				yt = y0;
				h = y1-y0;
			} else {
				yt = y1;
				h = y0-y1;
			}
			painter.drawPixmap(xt, yt, w, h, (*oldCopy));
		}
	painter.end();
	update();
}

QSelectionLabel::~QSelectionLabel() {
	if (oldCopy) 
		delete oldCopy;
}

void QSelectionLabel::keyPressEvent ( QKeyEvent * event ) {
    if (pixmap()->isNull()) {
        event->ignore();
        return;
    }
    event->accept();
    QLabel::keyPressEvent(event);
}

void QSelectionLabel::keyReleaseEvent ( QKeyEvent * event ) {
      if (pixmap()->isNull()) {
          event->ignore();
        return;
    }
    event->accept();
    QLabel::keyReleaseEvent(event);
}
