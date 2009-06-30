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

#include <QApplication>
#include <QTranslator>
#include <QLocale>
#include "mainform.h"

int main(int argc, char *argv[])
{
	QApplication app(argc, argv);
	QTranslator translator;
        if (!translator.load("yagf_" + QLocale::system().name(), "/usr/local/share/yagf/translations"))
	  translator.load("yagf_" + QLocale::system().name(), "/usr/share/yagf/translations");
	app.installTranslator(&translator);
        QTranslator translator2;
        translator2.load("qt_" + QLocale::system().name(), "/usr/share/qt4/translations");
        app.installTranslator(&translator2);
	MainForm * window = new MainForm;
	window->show();
	return app.exec();
}
