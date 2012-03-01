/*
    YAGF - cuneiform OCR graphical front-end
    Copyright (C) 2009-2012 Andrei Borovsky <anb@symmetrica.net>

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

#include "imageiomanager.h"
#include <QSize>
#include <QRgb>

ImageIOManager::ImageIOManager(QObject *parent) :
    QObject(parent)
{
    imgb2 = NULL;
    imgb4 = NULL;
    imgb8 = NULL;
    fragment = NULL;
}

ImageIOManager::~ImageIOManager()
{
    delete imgb2;
    delete imgb4;
    delete imgb8;
    delete fragment;
}

bool ImageIOManager::open(const QString &fileName)
{
    ir.setFileName(fileName);
    originalSize = ir.size();
    size.setWidth(originalSize.width()/2);
    size.setHeight(originalSize.height()/2);
    imgb2 = new QImage(size, QImage::Format_RGB32);
    file = fileName;
    ir.setScaledSize(size);
    if (ir.read(imgb2)) {
        makeSmall();
        return true;
    } else
        return false;
}


void ImageIOManager::makeSmall()
{
    int w = imgb2->width();
    int h = imgb4->height();
    QSize size1(w/2, h/2);
    QSize size2(size1->width()/2 + 1, size1->height()/2 + 1);
    imgb4 = new QImage(size1, QImage::Format_RGB32);
    imgb8 = new QImage(size2, QImage::Format_RGB32);
    bool turn = true;
    for (int i = 0; i < h; i += 2) {
        QRgb * line = (QRgb *) imgb2.scanLine(i);
        QRgb * line1 = (QRgb *) imgb4.scanLine(i/2);
        for (int j = 0; j < w; j += 2)
            line1[j >> 1] = line[j];

    }
    for (int i = 0; i < size1.height(); i += 2) {
        QRgb * line = (QRgb *) imgb4.scanLine(i);
        QRgb * line1 = (QRgb *) imgb8.scanLine(i/2);
        for (int j = 0; j < size1.width(); j += 2)
            line1[j >> 1] = line[j];

    }

}

QPixmap & ImageIOManager::pixmap(int sf)
{
    if (!imgb2)
        return pmb2;
    switch(sf) {
    case 1:
        fragment = new QPixmap(file);
        return * fragment;
    case 2:
        if (pmb2.isNull())
            pmb2.fromImage(imgb2);
        return pmb2;
    case 4:
        if (pmb4.isNull())
            pmb4.fromImage(imgb4);
        return pmb4;
    case 8:
        if (pmb8.isNull())
            pmb8.fromImage(imgb8);
        return pmb8;
    }
    return pmb2;
}

void ImageIOManager::clearFragment()
{
    delete fragment;
    fragment = NULL;
}

QPixmap * ImageIOManager::getFragment(const QRect &rect)
{
    ir.setScaledSize(originalSize);
    ir.setClipRect(rect);
    QImage img = ir.read();
    if (img.isNull())
        return NULL;
    if (fragment)
        delete fragment;
    fragment = new QPixmap();
    fragment->fromImage(img);
    return fragment;
}
