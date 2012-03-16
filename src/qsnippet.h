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

#ifndef QSNIPPET_H
#define QSNIPPET_H

#include <QListWidget>
#include <QListWidgetItem>
#include <QList>
#include <QRect>
#include <QPixmap>

typedef QList<QRect> BolockList;

class QSnippet : public QListWidgetItem
{
public:
    explicit QSnippet(QListWidget *parent = 0);
    bool addFile(const QString &name, const QImage * image = NULL);
    void setRotation(qreal rot);
    qreal getRotation();
    void setScale(double sc);
    double getScale();
    void setCrop1(const QRect &rect);
    QRect getCrop1();
    void setCrop2(const QRect &rect);
    QRect getCrop2();
    QString getName();
    BolockList * blocks();
signals:

public slots:
private:
    BolockList blockList;
    qreal rotation;
    double scale;
    QString name;
    QRect crop1;
    QRect crop2;
};

#endif // QSNIPPET_H
