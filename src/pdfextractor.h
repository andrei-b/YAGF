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

#ifndef PDFEXTRACTOR_H
#define PDFEXTRACTOR_H

#include <QObject>
#include <QStringList>

class PDFExtractor : public QObject
{
    Q_OBJECT
public:
    explicit PDFExtractor(QObject *parent = 0);
    void setCommandStringPaged(const QString &cmdStr);
    void setCommandStringEntire(const QString &cmdStr);
    void setSourcePDF(const QString &value);
    QString getSourcePDF();
    void setOutputDir(const QString &value);
    QString getOutputDir();
    void setStartPage(const QString &value);
    QString getStartPage();
    void setStopPage(const QString &value);
    QString getStopPage();
    void setResolution(const QString &value);
    QString getResolution();
    void setOutputPrefix(const QString &value);
    QString getOutputPrefix();
    void setOutputExtension(const QString &value);
    QString getOutputExtension();
    void virtual exec() = 0;
    static bool findProgram();
signals:
    void terminate();
    void killProcess();
    void terminateProcess();
    void addPage(QString pageName);
    void finished();
public slots:
    void cancel();
protected:
    void execInternal(const QString &command, const QStringList &arguments);
private:
    QString commandStringPaged;
    QString commandStringEntire;
    QString sourcePDF;
    QString outputDir;
    QString startPage;
    QString stopPage;
    QString resolution;
    QString outputPrefix;
    QString outputExtension;
    int lastPage;
    QStringList filters;
    bool canceled;
};

#endif // PDFEXTRACTOR_H
