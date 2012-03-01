/*
    YAGF - cuneiform and tesseract OCR graphical front-end
    Copyright (C) 2011 Andrei Borovsky <anb@symmetrica.net>

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


#include "analysis.h"
#include <math.h>

bool operator==(Rect r1, Rect r2)
{
    if (r1.x1 != r2.x1)
        return false;
    if (r1.y1 != r2.y1)
        return false;
    if (r1.x2 != r2.x2)
        return false;
    if (r1.y2 != r2.y2)
        return false;
    return true;
}

CCAnalysis::CCAnalysis(CCBuilder * builder)
{
	this->builder = builder;
	glyphCount = 0;
	mediumGlyphHeight = 0;
	mediumGlyphWidth = 0;
	mediumLetterSpace = 0;
	mediumWordSpace = 0;
	stringsCount = 0;	
}

CCAnalysis::~CCAnalysis()
{
	
}

void CCAnalysis::analize()
{
    extractComponents();
    classifyGlyphs();
    normalizeLines();
}

void CCAnalysis::extractComponents()
{
    components.clear();
    for (int y = 0; y < builder->height(); y++) {
        for (int x = 0; x < builder->width(); x++) {
            quint32 label = builder->label(x,y);
            if (label) {
                Rect r;
                r.x1 = builder->width();
                r.x2 = 0;
                r.y1 =  builder->height();
                r.y2 = 0;
                if (!components.contains(label))
                     components.insert(label, r);
                r = components.value(label);
                if (x < r.x1)
                    r.x1 = x;
                if (x > r.x2)
                    r.x2=x;
                if (y<r.y1)
                    r.y1 = y;
                if (y > r.y2)
                    r.y2 = y;
                components.remove(label);
                components.insert(label,r);
             }
        }
    }
    quint32 wacc, hacc, count;
    wacc = 0;
    hacc= 0;
    count = 0;
    foreach(Rect r, components.values()) {
        wacc += (r.x2 - r.x1);
        hacc +=(r.y2 - r.y1);
        count++;
    }
    quint32 wmed = wacc/count;
    quint32 hmed = hacc/count;
    //mediumGlyphWidth = wmed;
    /*foreach(quint32 k, components.keys()) {
        Rect r = components.value(k);
        if ((r.x2 - r.x1) > wmed + wmed)
            components.remove(k);
        else {
            if (((r.y2 - r.y1) < hmed) || ((r.y2 - r.y1) > hmed + hmed))
                components.remove(k);
        }
    }*/
    foreach(quint32 k, components.keys()) {
            Rect r = components.value(k);
            if ((r.x2 - r.x1) > 6*wmed)
                components.remove(k);
            else {
                if (((r.y2 - r.y1)*(r.x2 - r.x1) < 30) || ((r.y2 - r.y1) > 4*hmed))
                    components.remove(k);
            }
        }
    wacc = 0;
    hacc= 0;
    count = 0;
    foreach(Rect r, components.values()) {
        wacc += (r.x2 - r.x1);
        hacc +=(r.y2 - r.y1);
        count++;
    }
    if (count == 0) return;
    wmed = wacc/count;
    hmed = hacc/count;
    mediumGlyphWidth = wmed;
    mediumGlyphHeight = hmed;
}

int CCAnalysis::getGlyphBoxCount()
{
    return components.count();
}

Rect CCAnalysis::getGlyphBox(int index)
{
    return  components.values().at(index);
}

void CCAnalysis::classifyGlyphs()
{
    foreach (Rect r, components) {
        glyphField.insert(r.x1, r);
    }
}

TextLine CCAnalysis::extractLine()
{
    Rect first;
    //QPoint firstpt;
    TextLine line;
    for (int x =0; x < builder->width(); x++) {
        if (glyphField.values(x).count()) {
            first = glyphField.values(x).at(0);
            glyphField.remove(x, first);
            line.append(QPoint((first.x1+first.x2)/2, (first.y1+first.y2)/2));
            break;
        }
    }
    if (line.count()) {
        Rect temp = first;
        while (findAdjacent(temp) >= 0) {
            line.append(QPoint((temp.x1+temp.x2)/2, (temp.y1+temp.y2)/2));
        }
    }
    return line;
}

int CCAnalysis::findAdjacent(Rect &r)
{
    int startx = (r.x1+r.x2)/2;
    int xspan = (r.x2 - r.x1)*4;
    int ymid = (r.y1+r.y2)/2;
    int endx = startx + xspan;
    for (int x=startx; x <endx; x++) {
        foreach(Rect r1, glyphField.values(x)) {
            if ((ymid >= r1.y1) &&  (ymid <= r1.y2)) {
                r = r1;
                glyphField.remove(x, r1);
                return x;
            }
        }
    }
    return -1;
}

void CCAnalysis::normalizeLines()
{
    k = 0;
    int count = 0;
    TextLine l = extractLine();
    while (l.count()) {
        lines.append(l);
        if (l.count() > 4) {
            qreal d = l.at(l.count()-1).x() - l.at(1).x();
            if (d != 0) {
                k = k + ((qreal)(l.at(l.count()-1).y() - l.at(1).y()))/d;
                count++;
            }
        }
        //graphicsInput->drawLine(l.at(0).x()*2, l.at(0).y()*2, l.at(l.count()-1).x()*2,l.at(l.count()-1).y()*2);
        l = extractLine();
    }
    if (count)
        k = k/count;
}

Lines CCAnalysis::getLines()
{
    return lines;
}

qreal CCAnalysis::getK()
{
    return k;
}

void CCAnalysis::rotatePhi(qreal phi, const QPoint &c, QPoint &p)
{
    int x  = p.x() - c.x();
    int y = p.y() - c.y();
    int x1 = x*cos(phi) - y*sin(phi);
    int y1 = x*sin(phi) + y*cos(phi);
    p.setX(x1+c.x());
    p.setY(y1+c.y());
}

void CCAnalysis::rotateLines(qreal phi, const QPoint &c)
{
    for (int i = 0; i < lines.count(); i++) {
        TextLine l = lines.at(i);
        for (int j =0; j < l.count(); j++) {
            QPoint p = l.at(j);
            rotatePhi(phi, c, p);
            l.replace(j, p);
        }
        lines.replace(i, l);
    }
}

int CCAnalysis::getMediumGlyphWidth()
{
    return mediumGlyphWidth;
}

int CCAnalysis::getMediumGlyphHeight()
{
    return mediumGlyphHeight;
}

