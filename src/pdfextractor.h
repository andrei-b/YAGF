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
