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

bool operator==(GlyphInfo g1, GlyphInfo g2)
{
    if (g1.x != g2.x)
        return false;
    if (g1.y != g2.y)
        return false;
    if (g1.h != g2.h)
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

void CCAnalysis::analize(bool extractBars)
{
    extractComponents(extractBars);
    classifyGlyphs();
    normalizeLines();
}

Bars CCAnalysis::addBars()
{
    extractComponents(true);
    addBarsHorizontal();
    addBarsVertical();
    return bars;
}

void CCAnalysis::extractComponents(bool extractBars)
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

    if (extractBars) {
        foreach(quint32 k, components.keys()) {
                Rect r = components.value(k);
                int deltaX  = abs(r.x2 - r.x1); // TODO: remove abs() if not needed
                int deltaY = abs(r.y2 - r.y1); // TODO: remove abs() if not needed
                if ((deltaX > 10)&&(deltaY > 10)) continue;
                if (deltaX != 0) {
                    if (deltaY/deltaX >= BarRatio) {
                        components.remove(k);
                        bars.append(r);
                        continue;
                    }

                } else {
                    if (deltaY >= BarRatio) {
                        components.remove(k);
                        bars.append(r);
                        continue;
                    }
                }
                if (deltaY != 0) {
                    if (deltaX/deltaY >= BarRatio) {
                        components.remove(k);
                        bars.append(r);
                        continue;
                    }

                }  else {
                    if (deltaX >= BarRatio) {
                        components.remove(k);
                        bars.append(r);
                        continue;
                    }
                }
        }

    }

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
            line.append(ginfo((first.x1+first.x2)/2, (first.y1+first.y2)/2, abs(first.y1-first.y2)));
            break;
        }
    }
    if (line.count()) {
        Rect temp = first;
        while (findAdjacent(temp) >= 0) {
            line.append(ginfo((temp.x1+temp.x2)/2, (temp.y1+temp.y2)/2, abs(temp.y1-temp.y2)));
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
            if (abs (l.last().h - l.first().h) < 3) {
                qreal d = l.last().x - l.first().x;
                if (d != 0) {
                    k = k + ((qreal)(l.last().y - l.first().y))/d;
                    count++;
                }
            } else {
                int lastX = l.at(l.count()-1).x;
                int lastY = l.at(l.count()-1).y;
                int lastH = l.at(l.count()-1).h;
                int firstX = l.at(1).x;
                int firstY = l.at(1).y;
                int firstH = l.at(1).h;
                if (abs (lastH - firstH) < 3) {
                    qreal d = lastX - firstX;
                    if (d != 0) {
                        k = k + ((qreal)(lastY - firstY))/d;
                        count++;
                    }
                }
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

void CCAnalysis::addBarsHorizontal()
{
    bool * li = new bool[builder->height()];
    for (int i = 0; i < builder->height(); i++)
        li[i] = false;
    foreach (TextLine tl, lines) {
        if (tl.count() == 1) continue;
        foreach(GlyphInfo gi, tl)
            for (int i = gi.y - gi.h/2;  i < gi.y + gi.h/2; i++)
                li[i] = true;
    }
    int him = 0;
    int hlm = 0;
    int lcount = 0;
    int icount = 0;
    int chl = 0;
    int chi = 0;
    for (int i= 0; i < builder->height(); i++) {
        if (!li[i]) { // empty space
            if (chl > 0) {
                hlm += chl;
                lcount++;
                chl = 0;
            }
            chi++;
        } else {
            if (chi > 0) {
                if (icount > 0)
                    him += chi;
                icount++;
                chi = 0;
            }
            chl++;
        }
    }
    if ((icount < 3)||(lcount == 0)) {
        delete[] li;
        return;
    }
    him -= chi;
    him = him/(icount-2);
    hlm /= lcount;
    int ilcount = 0;
    int llcount = 0;
    for (int i = 0; i < builder->height(); i++) {
        if (!li[i]) {
            /*if (llcount >= 1.5*hlm) {
                Rect r;
                r.x1 = 0;
                r.x2 = builder->width()-1;
                r.y1 = i - hlm/2;
                r.y2 = r.y1;
                bars.append(r);
                llcount = 0;
            }*/
            ilcount++;
        } else {
            if (ilcount >= 4*him) {
                Rect r;
                r.x1 = 0;
                r.x2 = builder->width()-1;
                r.y1 = 0;
                for (int j = i - 3*him; j < i; j++) {
                    if ((!li[j-1]) && (!li[j]) && (!li[j+1])) {
                        r.y1 = j;
                        break;
                    }
                }
                if (r.y1 > 0) {
                    r.y2 = r.y1;
                    bars.append(r);
                }
            }
            ilcount = 0;
            llcount++;
        }
    }
    delete[] li;
}

/*void CCAnalysis::addBarsHorizontal()
{
    bool * li = new bool[builder->height()];
    for (int i = 0; i < builder->height(); i++)
        li[i] = false;
    int hlm = 0;
    foreach (TextLine tl, lines)
        hlm += tl.first().h;
    hlm /= lines.count();
    foreach (TextLine tl, lines) {
        if (tl.count() < 3)
            if (tl.first().x > lines.first().first().x + 64)
                continue;

        for (int i = (tl.first().y - hlm < 0 ? 0 : tl.first().y - hlm); i < (tl.last().y + hlm > builder->height() ? builder->height() : tl.last().y + hlm); i++)
            li[i] = true;
        //for (int i = (tl.last().y - tl.last().h < 0 ? 0 : tl.last().y - tl.last().h); i < (tl.last().y + tl.last().h > builder->height() ? builder->height() : tl.last().y + tl.last().h); i++)
          //  li[i] = true;
    }
    int fcount = 0;
    for (int i = 0; i < builder->height(); i++) {
        if (!li[i]) fcount++;
        else {
            if (fcount >= hlm*1.5) {
                Rect r;
                r.x1 = 0;
                r.x2 = builder->width()-1;
                r.y1 = i - hlm/2;
                r.y2 = r.y1;
                bars.append(r);
                fcount = 0;
            }
        }
    }
    delete[] li;
}*/

void CCAnalysis::addBarsVertical()
{
    bool * li = new bool[builder->width()];
    for (int i = 0; i < builder->width(); i++)
        li[i] = false;
    foreach (TextLine tl, lines) {
        if (tl.count() < 3) continue;
        foreach (GlyphInfo gi, tl) {
            for (int i = (gi.x - gi.h > 0 ? gi.x - gi.h : 0); (i < builder->width()) && (i < gi.x + gi.h); i++)
                li[i] = true;
        }

        /*for (int i = (tl.first().x - tl.first().h < 0 ? 0 : tl.first().x - tl.first().h); i < tl.last().x; i++) //(tl.last().x < builder->width() ? tl.last().x : builder->width()); i++)
            li[i] = true;
//        for (int i = (tl.last().x - tl.last().h < 0 ? 0 : tl.last().x - tl.last().h); i < (tl.last().x + tl.last().h > builder->width() ? builder->width() : tl.last().x + tl.last().h); i++)
  //          li[i] = true;*/
    }
    int fcount = 0;
    for (int i = 0; i < builder->width(); i++) {
        if (!li[i]) fcount++;
        else {
            if (fcount >= mediumGlyphWidth ) { //mediumGlyphHeight) {
                Rect r;
                for (int j = i - (fcount + mediumGlyphWidth); j < i; j++) {
                    if ((!li[j-1])&&(!li[j])&&(!li[j+1]))
                    r.x1 = j;
                    r.x2 = r.x1;
                    r.y1 = 0;
                    r.y2 = builder->height()-1;
                    bars.append(r);
                    break;
                }
                fcount = 0;
            }
        }
    }
    delete[] li;
}

void CCAnalysis::rotateLines(qreal phi, const QPoint &c)
{
    for (int i = 0; i < lines.count(); i++) {
        TextLine l = lines.at(i);
        for (int j =0; j < l.count(); j++) {
            QPoint p = QPoint(l.at(j).x, l.at(j).y);
            rotatePhi(phi, c, p);
            ginfo g = l.at(j);
            g.x = p.x();
            g.y = p.y();
            l.replace(j, g);
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

ginfo::ginfo(int a1, int a2, int a3)
{
    x = a1;
    y = a2;
    h = a3;
}

