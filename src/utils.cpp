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

#include <QString>
#include <QStringList>
#include <QFileInfo>
#include <QByteArray>
#include <QDir>
#include <stdlib.h>
#include "utils.h"

QString extractFileName(const QString &path)
{
    QFileInfo fi(path);
    return fi.fileName();
}

QString extractFilePath(const QString &path)
{
    QFileInfo fi(path);
    QString s = fi.dir().path();
    if (!s.endsWith("/"))
        s += '/';
    return s;
}

QString extractDigits(const QString &fn)
{
    bool extracting = FALSE;
    QString result = "";
    for (int i = 0; i < fn.size(); i++)
        if ((fn.at(i) >= '0') && (fn.at(i) <= '9')) {
            extracting = TRUE;
            result += fn.at(i);
        } else {
            if (extracting) break;
        }
    return result;
}

bool findProgram(const QString &name)
{
    QStringList sl = QString(getenv("PATH")).split(":");
    QFileInfo fi;
    for (int i = 0; i < sl.count(); i++) {
        fi.setFile(sl.at(i), name);
        if (fi.exists())
            return true;
    }
    return false;
}
