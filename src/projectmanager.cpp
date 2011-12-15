/*
    YAGF - cuneiform and tesseract OCR graphical front-end
    Copyright (C) 2011 Andrei Borovsky <anb@symmetrica.net>

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

#include "projectmanager.h"
#include <QFile>
#include <QTextStream>

ProjectManager::ProjectManager(SnippetList *sl) : QObject(0)
{
    snippets = sl;
}

ProjectManager::~ProjectManager()
{

}

void ProjectManager::save(const QString &fileName)
{
    QFile file(fileName);
     if (file.open(QFile::WriteOnly | QFile::Truncate)) {
         QTextStream out(&file);
         out << "YAGF:1" << endl;
         foreach (QSnippet * s, *snippets) {
            out << "page" << endl;
            out << s->getName() << endl;
            out << s->getRotation() << endl;
            out << s->getScale() << endl;
            foreach(QRect b, *(s->blocks())) {
                out << b.x() << b.y() << b.width() << b.height() << endl;
            }
         }
     }
}

void ProjectManager::load(const QString &fileName)
{
}
