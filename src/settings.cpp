/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
*/

#include "settings.h"
#include "utils.h"
#include <QProcessEnvironment>
#include <QDir>
#include <QLocale>
#include <QVariant>

Settings::Settings()
{
    settings = NULL;
}

Settings::~Settings()
{
    delete settings;
}

void Settings::readSettings(const QString &path)
{
    mPath = path;
    mPath = mPath.append("yagf.ini");
    settings = new QSettings(mPath, QSettings::IniFormat);
    lastDir = settings->value("mainwindow/lastDir").toString();
    lastOutputDir = settings->value("mainwindow/lastOutputDir").toString();
    QString defEngine;
    if (findProgram("tesseract")&&(!findProgram("cuneiform")))
        defEngine = "tesseract";
    else
        defEngine = "cuneiform";
    QString engine = settings->value("ocr/engine", QVariant(defEngine)).toString();
    if (engine == "cuneiform")
        selectedEngine = UseCuneiform;
    else
        selectedEngine = UseTesseract;
    language = settings->value("ocr/language",  selectDefaultLanguageName()).toString();
    //selectLangsBox->setCurrentIndex(selectLangsBox->findData(QVariant(language)));
    outputFormat = settings->value("ocr/outputFormat", QString("text")).toString();
    if (outputFormat == "") outputFormat = "text";
    checkSpelling = settings->value("mainWindow/checkSpelling", bool(true)).toBool();
    bool ok;
    fontSize = settings->value("mainWindow/fontSize", int(12)).toInt(&ok);
    findTessDataPath();
    tessdataPath = settings->value("ocr/tessData", QVariant(tessdataPath)).toString();
    if (tessdataPath.isEmpty())
        findTessDataPath();
    cropLoaded =  settings->value("processing/crop1", QVariant(true)).toBool();
    size = settings->value("mainwindow/size", QSize(800, 600)).toSize();
    iconSize = settings->value("mainwindow/iconSize", QSize(48, 48)).toSize();
    position = settings->value("mainwindow/pos", QPoint(0, 0)).toPoint();
    fullScreen = settings->value("mainwindow/fullScreen", QVariant(false)).toBool();

}

void Settings::writeSettings()
{
    settings->setValue("mainwindow/size", size);
    settings->setValue("mainwindow/iconSize", iconSize);
    settings->setValue("mainwindow/pos", position);
    settings->setValue("mainwindow/fullScreen", fullScreen);
    settings->setValue("mainwindow/lastDir", lastDir);
    settings->setValue("mainWindow/checkSpelling", checkSpelling);
    settings->setValue("mainwindow/lastOutputDir", lastOutputDir);
    settings->setValue("mainWindow/fontSize", fontSize);
    settings->setValue("ocr/language", language);
    //settings->setValue("ocr/singleColumn", singleColumn);
    settings->setValue("ocr/outputFormat", outputFormat);
    QString engine = selectedEngine == UseCuneiform ? QString("cuneiform") : QString("tesseract");
    settings->setValue("ocr/engine", engine);
    settings->setValue("ocr/tessData", tessdataPath);
    settings->setValue("processing/crop1", cropLoaded);
    settings->sync();
}

QString Settings::getLanguage()
{
    return language;
}

QString Settings::getOutputFormat()
{
    return outputFormat;
}

QString Settings::getLastDir()
{
    return lastDir;
}

QString Settings::getLastOutputDir()
{
    return lastOutputDir;
}

bool Settings::getCheckSpelling()
{
    return checkSpelling;
}

QString Settings::getTessdataPath()
{
    if (!tessdataPath.endsWith("/"))
        tessdataPath = tessdataPath.append("/");
    return tessdataPath;
}

SelectedEngine Settings::getSelectedEngine()
{
    return selectedEngine;
}

QSize Settings::getSize()
{
    return size;
}

QPoint Settings::getPosition()
{
    return position;
}

bool Settings::getFullScreen()
{
    return fullScreen;
}

int Settings::getFontSize()
{
    return fontSize;
}

bool Settings::getCropLoaded()
{
    return cropLoaded;
}

void Settings::setLanguage(const QString &value)
{
    language = value;
}

void Settings::setOutputFormat(const QString &value)
{
    outputFormat = value;
}

void Settings::setLastDir(const QString &value)
{
    lastDir = value;
}

void Settings::setLastOutputDir(const QString &value)
{
    lastOutputDir = value;
}

void Settings::setCheckSpelling(const bool value)
{
    checkSpelling = value;
}

void Settings::setTessdataPath(const QString &value)
{
    tessdataPath = value;
}

void Settings::setSelectedEngine(const SelectedEngine value)
{
    selectedEngine = value;
}

void Settings::setSize(const QSize &value)
{
    size = value;
}

void Settings::setPosition(const QPoint &value)
{
    position = value;
}

void Settings::setFullScreen(const bool value)
{
    fullScreen = value;
}

void Settings::setFontSize(const int &value)
{
    fontSize = value;
}

void Settings::setCropLoaded(const bool value)
{
    cropLoaded = value;
}

void Settings::findTessDataPath()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (env.contains("TESSDATA_PREFIX")) {
        tessdataPath = env.value("TESSDATA_PREFIX");
        return;
    }
    QDir dir;
    dir.setPath("/usr/share/tessdata");
    if (dir.exists()) {
        tessdataPath = "/usr/share/";
        return;
    }
    dir.setPath("/usr/local/share/tessdata");
    if (dir.exists()) {
        tessdataPath = "/usr/local/share/";
        return;
    }
    dir.setPath("/usr/local/share/tesseract-ocr/tessdata");
    if (dir.exists()) {
        tessdataPath = "/usr/local/share/tesseract-ocr/";
        return;
    }
    dir.setPath("/usr/share/tesseract-ocr/tessdata");
    if (dir.exists()) {
        tessdataPath = "/usr/share/tesseract-ocr/";
        return;
    }
    tessdataPath.clear();
    return;
}

QString Settings::selectDefaultLanguageName()
{
    QLocale loc = QLocale::system();
    QString name = "eng";
    switch (loc.language()) {
        case QLocale::Bulgarian:
            name = "bul";
            break;
        case QLocale::Czech:
            name = "cze";
            break;
        case QLocale::Danish:
            name = "dan";
            break;
        case QLocale::German:
            name = "ger";
            break;
        case QLocale::Dutch:
            name = "dut";
            break;
        case QLocale::Russian:
            {
                if (selectedEngine == UseCuneiform)
                    name = "ruseng";
                else
                    name = "rus";
            }
            break;
        case QLocale::English:
            name = "eng";
            break;
        case QLocale::Spanish:
            name = "spa";
            break;
        case QLocale::French:
            name = "fra";
            break;
        case QLocale::Hungarian:
            name = "hun";
            break;
        case QLocale::Italian:
            name = "ita";
            break;
        case QLocale::Latvian:
            name = "lav";
            break;
        case QLocale::Lithuanian:
            name = "lit";
            break;
        case QLocale::Polish:
            name = "pol";
            break;
        case QLocale::Portuguese:
            name = "por";
            break;
        case QLocale::Romanian:
            name = "rum";
            break;
        case QLocale::Swedish:
            name = "swe";
            break;
        case QLocale::Serbian:
            name = "srp";
            break;
        case QLocale::Slovenian:
            name = "slo";
            break;
        case QLocale::Ukrainian:
            name = "ukr";
        case QLocale::Finnish:
            name = "fin";
            break;
        case QLocale::Greek:
            name = "ell";
            break;
        case QLocale::Hebrew:
            name = "heb";
            break;
        default:
            name = "eng";
            break;
    }
    return name;
}

QSize Settings::getIconSize()
{
    return iconSize;
}

void Settings::setIconSize(const QSize &value)
{
    iconSize = value;
}
