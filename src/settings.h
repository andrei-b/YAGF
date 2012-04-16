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

#ifndef _SETTINGS_H_
#define _SETTINGS_H_

#include <QString>
#include <QSettings>
#include <QSize>
#include <QPoint>


enum SelectedEngine {
    UseCuneiform,
    UseTesseract
};

class Settings
{
public:
  Settings();
  ~Settings();
  void readSettings(const QString &path);
  void writeSettings();
  QString getLanguage();
  QString getOutputFormat();
  QString getLastDir();
  QString getLastOutputDir();
  bool getCheckSpelling();
  QString getTessdataPath();
  SelectedEngine getSelectedEngine();
  QSize getSize();
  QSize getIconSize();
  QPoint getPosition();
  bool getFullScreen();
  int getFontSize();
  bool getCropLoaded();
  void setLanguage(const QString &value);
  void setOutputFormat(const QString &value);
  void setLastDir(const QString &value);
  void setLastOutputDir(const QString &value);
  void setCheckSpelling(const bool value);
  void setTessdataPath(const QString &value);
  void setSelectedEngine(const SelectedEngine value);
  void setSize(const QSize &value);
  void setIconSize(const QSize &value);
  void setPosition(const QPoint &value);
  void setFullScreen(const bool value);
  void setFontSize(const int &value);
  void setCropLoaded(const bool value);
private:
  void findTessDataPath();
  QString selectDefaultLanguageName();
private:
  QString language;
  QString outputFormat;
  QString lastDir;
  QString lastOutputDir;
  bool checkSpelling;
  QString tessdataPath;
  SelectedEngine selectedEngine;
  QSize size;
  QSize iconSize;
  QPoint position;
  bool fullScreen;
  int fontSize;
  bool cropLoaded;

  QString mPath;
  QSettings * settings;
};

#endif
