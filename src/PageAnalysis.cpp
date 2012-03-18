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

#include "PageAnalysis.h"
#include "CCAnalysis.h"
#include "ccbuilder.h"
#include "analysis.h"
#include <QImage>
#include <QPixmap>
#include <QRect>
#include <QColor>
#include <QList>

void inline fillLine(QRgb * line, int length, QRgb value)
{
    for  (int j = 0; j < length; j++)
        line[j] = value;
}

PageAnalysis::PageAnalysis(QImage &img)
{
    m_image = &img;
    pointList = new QPointList();
}

PageAnalysis::~PageAnalysis()
{
    delete m_image;
    delete pointList;
}

void PageAnalysis::setBlack(QRgb color)
{
    tBlack = color;
}

void PageAnalysis::setBlackDeviance(int d)
{
    blackDeviance = d;
}

void PageAnalysis::setWhiteDeviance(int d)
{
    whiteDeviance = d;
}

void PageAnalysis::findTWhite()
{
    tWhite = qRgb(240,240,240);
    return;
    long int accumulator = 0;
    for (int i = 0; i < m_image->height(); i++) {
        QRgb * line = (QRgb *) m_image->scanLine(i);
        for (int j = 0; j < m_image->width(); j++) {
            int brightness = qBlue(line[j])+qRed(line[j])+qGreen(line[j]);
            if (755 - brightness < whiteDeviance) {
                accumulator = 0;
                break;
            } else {
                accumulator += line[j];
            }
        }
            if (accumulator != 0) {
        //        tWhite = line[m_image->width()/2];
                return;
            }
    }
    tWhite = 0xff888888;
}

bool PageAnalysis::isLineBlack(int index)
{
    int nonBlackPixels = 0;
    QRgb * line = (QRgb *) m_image->scanLine(index);
    int lastPixel = 0;
    int stripeLength = 0;
    for  (int j = 0; j < m_image->width(); j++) {
        int brightness = qBlue(line[j])+qRed(line[j])+qGreen(line[j]);
        if (brightness > blackDeviance) {
                nonBlackPixels++;
                if (lastPixel == j - 1) {
                    lastPixel = j;
                    stripeLength++;
                } else {
                    if (stripeLength == 0)
                        lastPixel = j;
                }
        }
    }
    if (!nonBlackPixels) return false;
    if (stripeLength >= 50)
        return false;
    int w = m_image->width();
    return ((w*10)/nonBlackPixels) > 25;
}

void PageAnalysis::removeBlackLines()
{

    newTop = 0;
    for (int i = 0; i < m_image->height(); i++) {
        if (isLineBlack(i)) {
            QRgb * line = (QRgb *) m_image->scanLine(i);
            fillLine(line, m_image->width(), tWhite);
            newTop = i +1;
        } else {
            bool anyBlack = false;
            for (int j = i; j < (m_image->height() - i < 10 ? m_image->height() : i + 10); j++)
                if (isLineBlack(j))
                    anyBlack = true;
            if (anyBlack) {
                fillLine((QRgb *) m_image->scanLine(i), m_image->width(), tWhite);
                newTop = i+1;
            } else break;
        }
    }
    newBottom = m_image->height() - 1;
    for (int i = m_image->height() - 1; i >= 0; i--) {
        if (isLineBlack(i)) {
            QRgb * line = (QRgb *) m_image->scanLine(i);
            fillLine(line, m_image->width(), tWhite);
            newBottom = i -1;
        } else  {
            bool anyBlack = false;
            for (int j = i; j >= (i < 10 ? 0 : i - 10); j--)
                if (isLineBlack(j))
                    anyBlack = true;
            if (anyBlack) {
                fillLine((QRgb *) m_image->scanLine(i), m_image->width(), tWhite);
                newBottom = i+1;
            } else break;
        }
    }
    if (newBottom < m_image->height() -2) {
        fillLine((QRgb *) m_image->scanLine(m_image->height() -1), m_image->width(), tWhite);
        fillLine((QRgb *) m_image->scanLine(m_image->height() -2), m_image->width(), tWhite);
    }
}
int PageAnalysis::findFirstNonBalckPixel(int index)
{
    QRgb * line = (QRgb *) m_image->scanLine(index);
    for (int i = 0; i < m_image->width(); i++) {
        int brightness = qBlue(line[i])+qRed(line[i])+qGreen(line[i]);
        if (765 - brightness < whiteDeviance)
                return i;
    }
    return m_image->width() - 1;
}

int PageAnalysis::findFirstNonBalckPixelBackwards(int index)
{
    QRgb * line = (QRgb *) m_image->scanLine(index);
    for (int i =  m_image->width() - 1; i>=0; i--) {
        int brightness = qBlue(line[i])+qRed(line[i])+qGreen(line[i]);
        if (765 - brightness < whiteDeviance)
                 return i;
    }
    return 0;
}

bool PageAnalysis::Process()
{
    findTWhite();
    removeBlackLines();
    removeBlackSideStripes();
    int ymiddle = (newBottom-newTop)/2;
    if (ymiddle < 50)
            return false;
    if (newRight - newLeft < 100)
        return false;
    return true;
}

QRect PageAnalysis::getCoords()
{
    return QRect(newLeft, newTop, newRight - newLeft, newBottom-newTop);
}

void PageAnalysis::removeBlackSideStripes()
{
    newLeft = m_image->width();
    newRight = 0;
    int nm = 0;
    for (int i = newTop; i < newBottom; i++) {
        QRgb * line = (QRgb *) m_image->scanLine(i);
        nm = findFirstNonBalckPixel(i);
        //if (nm < m_image->width()/2)
            pointList->append(QPoint(nm, i));
        if (newLeft > nm)
            newLeft = nm;
        for (int j = 0; j < nm; j++) {
            line[j] = tWhite;
        }

        nm = findFirstNonBalckPixelBackwards(i);;
        pointList->append(QPoint(nm, i));
        if (newRight < nm)
            newRight = nm;
        for (int j = m_image->width()-1; j > nm; j--) {
            line[j] = tWhite;
        }
    }
}

 QPointList * const PageAnalysis::getPoints()
{
     return pointList;
 }

 QImage PageAnalysis::getImage()
 {
     return *m_image;
 }

 void BlockSplitter::setImage(const QImage &image, qreal rotation, qreal scale)
 {
     img = image;
     m_rotate = rotation;
     m_scale = scale;
 }

 QRect BlockSplitter::getRootBlock(const QImage &image)
 {
     QImage img1 = image;
     QRect result = blockAllText();
     RotationCropper rc(&img1, QColor("white").rgb(), generalBr);
     QRect r = rc.crop();
     result.setWidth(result.width() + r.x());
     result.setX(result.x() + r.x());
     result.setHeight(result.height()+r.y());
     result.setY(result.y() + r.y());
     return result;
 }

 QRect BlockSplitter::blockAllText()
 {
     qreal x = img.width() / 2;
     qreal y = img.height() / 2;
     img = img.transformed(QTransform().translate(-x, -y).rotate(m_rotate).translate(x, y), Qt::SmoothTransformation);
     if (!img.isNull()) {
         CCBuilder * cb = new CCBuilder(img);
         cb->setGeneralBrightness(360);
         cb->setMaximumColorComponent(100);
         cb->labelCCs();
         CCAnalysis * an = new CCAnalysis(cb);
         an->analize();
  //       an->rotateLines(-atan(an->getK()));
         Lines lines = an->getLines();
         foreach(TextLine l, lines)
             if (l.count() < 3)
                 lines.removeOne(l);
         //QPoint orig;
         //graphicsInput->imageOrigin(orig);
         int minX = 100000;
         int minY = 100000;
         int maxX = 0;
         int maxY = 0;
         for (int i =0; i < lines.count(); i++) {
             int x1 = lines.at(i).at(0).x;
             int y1 = lines.at(i).at(0).y;
             int x2 = lines.at(i).at(lines.at(i).count()-1).x;
             int y2 = lines.at(i).at(lines.at(i).count()-1).y;
             //graphicsInput->drawLine(x1,y1,x2,y2);
             if (x1 > x2) {
                 int t = x2;
                 x2 = x1;
                 x1 = t;
             }
             minX = minX < x1 ? minX : x1;
             maxX = maxX > x2 ? maxX : x2;
             if (y1 > y2) {
                 int t = y2;
                 y2 = y1;
                 y1 = t;
             }
             minY = minY < y1 ? minY : y1;
             maxY = maxY > y2 ? maxY : y2;
         }
         minX = minX  - 2*an->getMediumGlyphWidth();
         maxX =maxX + 2*an->getMediumGlyphWidth();
         minY = minY - 2*an->getMediumGlyphHeight();
         maxY = maxY + 2*an->getMediumGlyphHeight();
         //graphicsInput->clearBlocks();
         //graphicsInput->addBlock(QRectF(ox + minX*2*sideBar->getScale(), oy + minY*2*sideBar->getScale(), (maxX-minX)*2*sideBar->getScale(), (maxY-minY)*2*sideBar->getScale()));

         delete an;
         generalBr = cb->getGB();
         delete cb;
         return QRect(minX*2*m_scale, minY*2*m_scale, (maxX-minX)*2*m_scale, (maxY-minY)*2*m_scale);
     }
 }

