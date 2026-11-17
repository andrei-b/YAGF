#pragma once

#include <QObject>
#include <QProcess>
#include <QTemporaryDir>

// Owns external programs and their temporary files. The UI never calls them directly.
class DocumentProcessor : public QObject {
    Q_OBJECT
public:
    explicit DocumentProcessor(QObject *parent = nullptr);
    bool isBusy() const;
    void openPdf(const QString &path);
    void renderPdfPage(int pageNumber);
    void recognize(const QString &imagePath, const QString &language);
    void cancel();
signals:
    void busyChanged(bool busy);
    void pdfOpened(int pageCount);
    void pageRendered(int pageNumber, const QString &imagePath);
    void recognized(const QByteArray &tsv);
    void cancelled();
    void failed(const QString &message);
private:
    enum class Job { None, PdfInfo, PdfRender, Ocr };
    void start(Job job, const QString &program, const QStringList &arguments);

    QProcess process;
    QTemporaryDir temporaryDir;
    Job job = Job::None;
    QString pdfSource;
    QString renderPrefix;
    int requestedPage = 1;
    bool cancellationRequested = false;
};
