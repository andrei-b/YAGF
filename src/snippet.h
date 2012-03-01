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


#ifndef SNIPPET_H
#define SNIPPET_H

#include <QWidget>
#include <QString>
#include <QPixmap>
#include <QList>
#include <QRectF>

typedef QList<QRectF> BlockList;

namespace Ui {
    class Snippet;
}

class Snippet : public QWidget
{
    Q_OBJECT

public:
    explicit Snippet(QWidget *parent = 0);
    ~Snippet();
    QString getFullName() const;
    QString getName() const;
    void setImage(const QPixmap &pixmap, const QString &name);
    BlockList &getBlocks();
    int getRotation();
    void setRotation(const int rotation);
    void setScale(const float scale);
    float getScale();
signals:
    void selected(QString name);
private:
    Ui::Snippet *ui;
    QString name;
    BlockList blocks;
    int rotation;
    float scale;
};

#endif // SNIPPET_H
