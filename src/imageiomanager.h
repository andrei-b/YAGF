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

#ifndef IMAGEIOMANAGER_H
#define IMAGEIOMANAGER_H

#include <QObject>
#include <QImageReader>
#include <QPixmap>

class ImageIOManager : public QObject
{
    Q_OBJECT
public:
    explicit ImageIOManager(QObject *parent = 0);
    ~ImageIOManager();
    bool open(const QString &fileName);
    QPixmap & pixmap(int sf = 2);
    void clearFragment();
signals:

public slots:

private:
    void makeSmall();
    QString file;
    QImageReader ir;
    QImage *imgb2;
    QImage *imgb4;
    QImage *imgb8;
    QPixmap pmb2;
    QPixmap pmb4;
    QPixmap pmb8;
    QPixmap * fragment;
};

#endif // IMAGEIOMANAGER_H
