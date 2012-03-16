/*
    YAGF - cuneiform and tesseract OCR graphical front-ends
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

#include "src/qsnippet.h"

QSnippet::QSnippet(QListWidget *parent) :
    QListWidgetItem(parent)
{
    rotation = 0;
    scale = 1;

}

bool QSnippet::addFile(const QString &name, const QImage * image)
{
    if (!image) {
        QImage img(name);
        if (img.isNull())
            return false;
        setIcon(QPixmap::fromImage(img));
    }
    else
        setIcon(QPixmap::fromImage(*image));
    this->name = name;
    setToolTip(name);
    return true;
}

void QSnippet::setRotation(qreal rot)
{
    rotation = rot;
}

qreal QSnippet::getRotation()
{
    return rotation;
}

void QSnippet::setScale(double sc)
{
    scale = sc;
}

double QSnippet::getScale()
{
    return scale;
}

QString QSnippet::getName()
{
    return name;
}

BolockList * QSnippet::blocks()
{
    return &blockList;
}

void QSnippet::setCrop1(const QRect &rect)
{
    crop1.setX(rect.x());
    crop1.setY(rect.y());
    crop1.setWidth(rect.width());
    crop1.setHeight(rect.height());
}

QRect QSnippet::getCrop1()
{
    return crop1;
}

void QSnippet::setCrop2(const QRect &rect)
{
    crop2 = rect;
}

QRect QSnippet::getCrop2()
{
    return crop2;
}
