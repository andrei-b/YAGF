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

#ifndef PAGEANALYSIS_H
#define PAGEANALYSIS_H

#include <QtCore>
#include <QColor>
#include <QList>
#include <QPoint>
#include "ycommon.h"

class QPixmap;
class QImage;
class QRect;



class PageAnalysis
{
public:
    PageAnalysis(QPixmap *pixmap);
    ~PageAnalysis();
    QPixmap getPixmap();
    void setBlack(QRgb color);
    void setBlackDeviance(int d);
    void setWhiteDeviance(int d);
    bool Process(); // if Process returns true, getCoords() probably returns right coords.
    QRect getCoords();
    QPointList * const getPoints();
private:
    QImage * m_image;
    QRgb tBlack;
    QRgb tWhite;
    int blackDeviance;
    int whiteDeviance;
    void findTWhite();
    bool isLineBlack(int index);
    void removeBlackLines();
    int findFirstNonBalckPixel(int index);
    int findFirstNonBalckPixelBackwards(int index);
    void removeBlackSideStripes();
    int newTop;
    int newBottom;
    int newLeft;
    int newRight;
    QPointList * pointList;
};


#endif
