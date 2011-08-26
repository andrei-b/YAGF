#include "pdfextractor.h"
#include <QThread>
#include <QProcess>
#include  <QStringList>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>

class PDFThread : public QThread
{
public:
    explicit PDFThread(PDFExtractor * parent) : QThread()
    {
        mparent = parent;
    }
    virtual void run()
    {
        QProcess process;
       // connect(this, SIGNAL(finished()), mparent, SIGNAL(finished()), Qt::QueuedConnection);
       // connect(this, SIGNAL(terminated()), mparent, SIGNAL(finished()), Qt::QueuedConnection);
        connect(mparent, SIGNAL(terminate()), this, SLOT(terminate()));
        process.start(command, arguments);
        process.waitForFinished(1600000);
    }
    void setProcess(const QString &cmd, const QStringList &args)
    {
        command = cmd;
        arguments.clear();
        arguments.append(args);
    }
private:
    QString command;
    QStringList arguments;
    PDFExtractor * mparent;
};

PDFExtractor::PDFExtractor(QObject *parent) :
    QObject(parent)
{
}

void PDFExtractor::setCommandStringPaged(const QString &cmdStr)
{
    commandStringPaged = cmdStr;
}

void PDFExtractor::setCommandStringEntire(const QString &cmdStr)
{
    commandStringEntire = cmdStr;
}

void PDFExtractor::setOutputDir(const QString &value)
{
    outputDir = value;
}

QString PDFExtractor::getOutputDir()
{
    return outputDir;
}

void PDFExtractor::setOutputExtension(const QString &value)
{
    outputExtension = value;
}

QString PDFExtractor::getOutputExtension()
{
    return outputExtension;
}

void PDFExtractor::setOutputPrefix(const QString &value)
{
    outputPrefix = value;
}

QString PDFExtractor::getOutputPrefix()
{
    return outputPrefix;
}

void PDFExtractor::setResolution(const QString &value)
{
    resolution = value;
}

QString PDFExtractor::getResolution()
{
    return resolution;
}

void PDFExtractor::setSourcePDF(const QString &value)
{
    sourcePDF = value;
}

QString PDFExtractor::getSourcePDF()
{
    return sourcePDF;
}

void PDFExtractor::setStartPage(const QString &value)
{
    startPage = value;
}

QString PDFExtractor::getStartPage()
{
    return startPage;
}

void PDFExtractor::setStopPage(const QString &value)
{
    stopPage = value;
}

QString PDFExtractor::getStopPage()
{
    return stopPage;
}

void PDFExtractor::cancel()
{
    emit terminate();
}

void PDFExtractor::execInternal(const QString &command, const QStringList &arguments)
{
    filters.clear();
    filters << QString("page*.%1").arg(getOutputExtension());
    PDFThread thread(this);
    thread.setProcess(command, arguments);
    thread.start();
    lastPage = 0;
    while (!thread.isFinished()) {
        QDir dir;
        dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
        dir.setSorting(QDir::Size | QDir::Reversed);
        dir.setSorting(QDir::Name);
        dir.setPath(outputDir);
        QFileInfoList fil;
        fil = dir.entryInfoList(filters, QDir::Files, QDir::Name);
        int lastI = 0;
        for (int i =lastPage; i < fil.count() - 1; i++) {
            emit addPage(fil.at(i).absoluteFilePath());
            lastI = i;
            QApplication::processEvents();
        }
        lastPage = lastI;
    }
    QDir dir;
    dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    dir.setSorting(QDir::Size | QDir::Reversed);
    dir.setSorting(QDir::Name);
    dir.cd(outputDir);
    QFileInfoList fil;
    fil = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    for (int i = 0; i < fil.count(); i++) {
        emit addPage(fil.at(i).absoluteFilePath());
        QApplication::processEvents();
    }
    emit finished();
}
