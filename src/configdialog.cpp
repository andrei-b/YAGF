/*
    YAGF - cuneiform and tesseract OCR graphical front-ends
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

#include "configdialog.h"
#include "ui_configdialog.h"
#include <QDir>
#include <QProcessEnvironment>

ConfigDialog::ConfigDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ConfigDialog)
{
    ui->setupUi(this);
}

ConfigDialog::~ConfigDialog()
{
    delete ui;
}

void ConfigDialog::setSelectedEngine(int value)
{
    if (!value)
        ui->radioButton->setChecked(true);
    else
        ui->radioButton_2->setChecked(true);
}

int ConfigDialog::selectedEngine()
{
    if (ui->radioButton->isChecked())
        return 0;
    return 1;
}

void ConfigDialog::setTessDataPath(const QString &value)
{
    ui->lineEdit->setText(value);
}

QString ConfigDialog::tessdataPath()
{
    return ui->lineEdit->text();
}
