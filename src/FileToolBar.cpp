/*
    YAGF - cuneiform OCR graphical front-end
    Copyright (C) 2009 Andrei Borovsky <anb@symmetrica.net>

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

#include "FileToolBar.h"
#include <QSize>
#include <QAction>
#include <QIcon>
#include <QPixmap>
#include <QPushButton>
#include <QFileDialog>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include "utils.h"

FileToolBar::FileToolBar(QWidget * parent):QToolBar(trUtf8("Loaded Images"), parent)
{
    buttonsAdded = false;
    this->setIconSize(QSize(64, 96));
    this->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    filesMap = new StringMap();
    saveButton = NULL;
    clearButton = NULL;
}

void FileToolBar::addFile(const QPixmap & pixmap, const QString & name)
{
    QAction * action;
    if (!buttonsAdded) {
        clearButton = new QPushButton(QIcon(":/clear.png"), trUtf8("Clear"), this);
        clearButton->setIconSize(QSize(24,24));
        connect(clearButton, SIGNAL(clicked()), this, SLOT(clearAll()));
        action = insertWidget(0, clearButton);
        saveButton = new QPushButton(QIcon(":/save_all.png"), trUtf8("Save..."), this);
        saveButton->setIconSize(QSize(24,24));
        connect(saveButton, SIGNAL(clicked()), this, SLOT(saveAll()));
        insertWidget(action, saveButton);
        buttonsAdded = true;
    }
    QPixmap pm = pixmap.scaledToHeight(96, Qt::FastTransformation);
    QString fn = extractFileName(name);
    if (!filesMap->contains(fn)) {
        filesMap->insert(fn, name);
        action = addAction(QIcon(pm), fn);
        connect(action, SIGNAL(triggered()), this, SLOT(selected()));
    }
}

void FileToolBar::selected()
{
    QString key = ((QAction *) sender())->text();
    QString path = filesMap->value(key);
    emit fileSelected(path);
}

void FileToolBar::saveAll()
{
    QFileDialog fd(this, trUtf8("Select a directory"), QDir::home().path());
    fd.setFileMode(QFileDialog::DirectoryOnly);
    if (fd.exec()) {
        QFile file;
        QMapIterator<QString, QString> i(*filesMap);
        while (i.hasNext()) {
             i.next();
             file.setFileName(i.value());
             QString newName = fd.selectedFiles().at(0) + '/' + extractFileName(i.value());
             if (file.exists(newName)) {
                    QPixmap icon;
                    icon.load(":/warning.png");
                    QMessageBox messageBox(QMessageBox::NoIcon, "YAGF", trUtf8("File %1 already exists. Do you want to replace it?").arg(newName),
                        QMessageBox::Yes|QMessageBox::No, this);
                    messageBox.setIconPixmap(icon);
                    int result = messageBox.exec();
                    if (result == QMessageBox::No)
                        continue;
                    else
                        file.remove(newName);
             }
             file.copy(newName);
         }

    }

}

void FileToolBar::clearAll()
{
    delete saveButton;
    delete clearButton;
    clear();
    buttonsAdded = false;
    filesMap->clear();
}
