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


#include "snippet.h"
#include "ui_snippet.h"
#include <QPalette>
#include <QFile>

Snippet::Snippet(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Snippet)
{
    setAttribute(Qt::WA_DeleteOnClose);
    ui->setupUi(this);
    QPalette p;
    p.setColor(ui->label->backgroundRole(), QColor(100, 30, 40));
    ui->label->setPalette(p);
    connect(ui->label, SIGNAL(selected(QString)), this, SIGNAL(selected(QString)));
    connect(ui->pushButton, SIGNAL(clicked()), this, SLOT(close()));
    rotation = 0;
    scale = 1.0;
}

Snippet::~Snippet()
{
    delete ui;
}

void Snippet::setImage(const QPixmap &pixmap, const QString &name)
{
    QPixmap pm;
    if (pixmap.isNull()) {
        pm.load(name);
        pm = pm.scaledToWidth(100);
    } else
    pm = pixmap.scaledToWidth(100);
    ui->label->setPixmap(pm);
    ui->label->setName(name);
    this->name  = name;
    QFile f(name);
    setToolTip(f.fileName());
}

QString Snippet::getName() const
{
    return name;
}

QString Snippet::getFullName() const
{
    return name;
}

BlockList & Snippet::getBlocks()
{
    return blocks;
}

int Snippet::getRotation()
{
    return rotation;
}

void Snippet::setRotation(const int rotation)
{
    this->rotation = rotation;
}

void Snippet::setScale(const float scale)
{
    this->scale = scale;
}

float Snippet::getScale()
{
    return scale;
}
