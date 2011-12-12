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

#include "ccbuilder.h"
#include <QRgb>
#define XDEBUG
#ifdef XDEBUG
#include <QtDebug>
#endif

CCBuilder::CCBuilder(QPixmap *pixmap, QObject *parent) :
    QObject(parent)
{
    if (!pixmap) {
        w = h = 0;
        return;
    }
    image = pixmap->toImage();
    image.convertToFormat(QImage::Format_RGB32);
    labels = new quint32 [image.height()*image.width()];
    maxlabel = 1;
    w = image.width();
    h = image.height();
}

CCBuilder::~CCBuilder()
{
    delete [] labels;
}

int CCBuilder::width()
{
    return w;
}

int CCBuilder::height()
{
    return h;
}


void CCBuilder::compactLabels()
{
    QList<quint32> l;
    l.append(0);
    for( int i = 0; i < w; i++)
        for( int j = 0; j < h; j++)
            if (!l.contains(label(i,j)))
                l.append(label(i,j));
    for( int i = 0; i < w; i++)
        for( int j = 0; j < h; j++)
            setLabel(i, j, l.indexOf(label(i,j)));
}

void CCBuilder::setGeneralBrightness(int value)
{
    generalBrightness =  value;
}

quint32 CCBuilder::label(int x, int y)
{
    int index = y*w+x;
    if (index < w*h)
        return labels[index];
    return 0;
}

int CCBuilder::labelChecked(int x, int y)
{
    if ((x<0)||(y<0))
        return 0;
    if ((x>=w)||(y>=h))
        return 0;
    return labels[y*w+x];
}

void CCBuilder::setLabel(int x, int y, int newValue)
{
    recolor.replace(label(x, y), newValue);
    labels[y*w+x] = newValue;
    didRecolor = true;
}

void CCBuilder::relabelLineLR(int y)
{
    for (int x = 0; x < w; x++) {
        if (labelChecked(x, y)) {
            int oc = labelChecked(x, y);
            if (recolor.at(labelChecked(x-1, y))) {
                int nc = recolor.at(labelChecked(x-1, y));
                if (nc < oc) {
                    setLabel(x, y, nc);
                    oc = nc;
                }
                if (recolor.at(labelChecked(x+1, y-1))) {
                    int nc = recolor.at(labelChecked(x+1, y-1));
                    if (nc < oc) {
                        setLabel(x, y, nc);
                        oc = nc;
                    }
                }
            } else {
                if (recolor.at(labelChecked(x, y-1))) {
                    int nc = recolor.at(labelChecked(x, y-1));
                    if (nc < oc) {
                        setLabel(x, y, nc);
                        oc = nc;
                    }
                }
                if (recolor.at(labelChecked(x-1, y-1))) {
                    int nc = recolor.at(labelChecked(x-1, y-1));
                    if (nc < oc) {
                        setLabel(x, y, nc);
                        oc = nc;
                    }
                }
                if (recolor.at(labelChecked(x+1, y-1))) {
                    int nc = recolor.at(labelChecked(x+1, y-1));
                    if (nc < oc) {
                        setLabel(x, y, nc);
                        oc = nc;
                    }
                }
            }
        }
    }
}

void CCBuilder::relabelLineRL(int y)
{
    for (int x = w-1; x >= 0; x--) {
        if (labelChecked(x, y)) {
            int oc = labelChecked(x, y);
            if (recolor.at(labelChecked(x+1, y))) {
                int nc = recolor.at(labelChecked(x+1, y));
                if (nc < oc) {
                    setLabel(x, y, nc);
                    oc = nc;
                }
                if (recolor.at(labelChecked(x-1, y+1))) {
                    int nc = recolor.at(labelChecked(x-1, y+1));
                    if (nc < oc) {
                        setLabel(x, y, nc);
                        oc = nc;
                    }
                }
            } else {
                if (recolor.at(labelChecked(x, y+1))) {
                    int nc = recolor.at(labelChecked(x, y+1));
                    if (nc < oc) {
                        setLabel(x, y, nc);
                        oc = nc;
                    }
                }
                if (recolor.at(labelChecked(x+1, y+1))) {
                    int nc = recolor.at(labelChecked(x+1, y+1));
                    if (nc < oc) {
                        setLabel(x, y, nc);
                        oc = nc;
                    }
                }
                if (recolor.at(labelChecked(x-1, y+1))) {
                    int nc = recolor.at(labelChecked(x-1, y+1));
                    if (nc < oc) {
                        setLabel(x, y, nc);
                        oc = nc;
                    }
                }
            }
        }
    }
}

bool CCBuilder::isForeground(QRgb value)
{
    int b = qRed(value) + qGreen(value) + qBlue(value);
    int maxc = qRed(value) > qGreen(value) ? qRed(value) : qGreen(value);
    if (maxc < qBlue(value))
	maxc = qBlue(value);
    //generalBrightness = (generalBrightness+b)/2;
    if ((b >= generalBrightness))// || (maxc >= maximumComponentBrightness))
        return false;

    return true;
}

void CCBuilder::setMaximumColorComponent(int value)
{
	maximumComponentBrightness = value;
}

void CCBuilder::initialScan()
{
    recolor.clear();
    recolor.append(0);
    scanFirstLineLR();
    for (int j = 1; j < h; j++) {
        labelLeftmostPoint(j);
        scanLineLR(j);
        labelRightmostPoint(j);
    }
}

void CCBuilder::backwardScan()
{
    for (int j = h-1; j >= 0; j--) {
        relabelLineRL(j);
    }
}

void CCBuilder::forwardScan()
{
    for (int j = 0; j < h; j++) {
        relabelLineLR(j);
    }
}

int CCBuilder::labelCCs()
{
    quint64 acc =0;
    for (int y =0; y < h; y++) {
        QRgb * line = (QRgb *) image.scanLine(y);
        for (int x = 0; x < w; x++) {
            int b = qRed(line[x]) + qGreen(line[x]) + qBlue(line[x]);
            acc += b;
        }
    }
    acc =acc/(h*w);
    setGeneralBrightness(acc);
    int count = 0;
    initialScan();
    didRecolor = true;
    int turn = 1;
    while (didRecolor) {
        didRecolor = false;
        if (turn)
            backwardScan();
        else
            forwardScan();
        turn = 1 - turn;
        count++;
    }
    return count;
}

void CCBuilder::scanFirstLineLR()
{
    if (!(h*w)) return;
    QRgb * line = (QRgb *) image.scanLine(0);
    if (isForeground(line[0])) {
            labels[0] = maxlabel;
            recolor.append(maxlabel);
            maxlabel++;
    } else
        labels[0] = 0;
    for (int i = 1; i < w; i++) {
        if (isForeground(line[i])) {
            if (labels[i-1])
                labels[i] = labels[i-1];
            else {
                labels[i] = maxlabel;
                recolor.append(maxlabel);
                maxlabel++;
            }
        } else
            labels[i] = 0;
    }
}

void CCBuilder::labelLeftmostPoint(int y)
{
    QRgb * line = (QRgb *) image.scanLine(y);
    if (isForeground(line[0])) {
        int prlabel = labels[w*(y-1)];
        if (prlabel)
            labels[w*y] = prlabel;
        else {
            if ((w > 1) && (labels[w*(y-1) + 1] > 0)) {
                labels[w*y] = labels[w*(y-1) + 1];
            } else {
                labels[w*y] = maxlabel;
                recolor.append(maxlabel);
                maxlabel++;
            }
        }
    } else
        labels[w*y] = 0;
}

void CCBuilder::labelRightmostPoint(int y)
{
    if (w < 2) return;
    QRgb * line = (QRgb *) image.scanLine(y);
    if (isForeground(line[w-1])) {
        if (labels[w*y-1])
            labels[w*(y+1) - 1] = labels[w*y-1];
        else {
            if (labels[w*y-2]) {
                labels[w*(y+1) - 1] = labels[w*y-2];
            } else
                if (labels[w*(y+1)-2]) {
                    labels[w*(y+1) - 1] = labels[w*(y+1)-2];
                } else {
                 labels[w*(y+1) - 1] = maxlabel;
                recolor.append(maxlabel);
                maxlabel++;
            }
        }
    } else
        labels[w*(y+1) - 1] = 0;
}

void CCBuilder::scanLineLR(int y)
{
    QRgb * line = (QRgb *) image.scanLine(y);
    for (int i = 1; i < w-1; i++) {
        labels[w*y + i] = 0;
        int nc;
        int oc;
        oc  = maxlabel;
        if (isForeground(line[i])) {
            nc = labels[w*(y - 1) + i];
            if (nc) {
                labels[w*y + i] = nc;
                oc = nc;
            }
            nc = labels[w*(y - 1) + i - 1];
            if ((nc) && (nc <= oc)) {
                labels[w*y + i] = nc;
                oc = nc;
            }
            nc = labels[w*(y - 1)  + i + 1];
            if ((nc) && (nc <= oc)) {
                labels[w*y + i] = nc;
                oc = nc;
            }
            nc = labels[w*y + i - 1];
            if ((nc) && (nc <= oc)) {
                labels[w*y + i] = nc;
                oc = nc;
            }
            if (!labels[w*y + i]) {
                labels[w*y + i] = maxlabel;
                recolor.append(maxlabel);
                maxlabel++;

            }
        }
    }
}
