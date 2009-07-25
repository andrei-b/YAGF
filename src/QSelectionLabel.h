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

#ifndef _QSELECTIONLABEL_H_
#define _QSELECTIONLABEL_H_

#include <QLabel>

class QRect;
class QMouseEvent;
class QPixmap;

class QSelectionLabel : public QLabel
{
public:
	QSelectionLabel(QWidget *parent=0, Qt::WindowFlags f=0);
	virtual ~QSelectionLabel();
	bool isSelectionEmpty();
	void setSelectionMode(bool mode);
	bool getSelectionMode();
	void resetSelection(bool redraw = false);
	QRect getSelectedRect();
protected:
    void mousePressEvent(QMouseEvent *ev);
    void mouseMoveEvent(QMouseEvent *ev);
    void mouseReleaseEvent(QMouseEvent *ev);
    void keyPressEvent ( QKeyEvent * event );
    void keyReleaseEvent ( QKeyEvent * event );
private:
	int x0, y0, x1, y1; 
	bool selecting;
	bool started;
	bool selected;
	QPixmap * oldCopy;
	void drawRect();
	void restoreRect();
};

#endif
