#pragma once

#include <QObject>
#include <QProcess>
#include <QRect>
#include <QPoint>
#include <QTemporaryDir>

namespace yagf::processing {

// Owns external programs and their temporary files. The UI never calls them directly.
class DocumentProcessor : public QObject {
    Q_OBJECT
public:
    explicit DocumentProcessor(QObject *parent = nullptr);
    ~DocumentProcessor() override;
    bool isBusy() const;
    void resetSource();
    void openPdf(const QString &path);
    void renderPdfPage(int pageNumber);
    void startThumbnails(int pageCount);
    void recognize(const QString &imagePath, const QString &language);
    void recognizeRegion(const QString &imagePath, const QString &language, const QRect &region);
    void cancel();
signals:
    void busyChanged(bool busy);
    void pdfOpened(int pageCount);
    void pageRendered(int pageNumber, const QString &imagePath);
    void thumbnailReady(int pageNumber, const QString &imagePath);
    void recognized(const QByteArray &tsv, const QPoint &offset, const QRect &region);
    void cancelled();
    void failed(const QString &message);
private:
    enum class Job { None, PdfInfo, PdfRender, Ocr };
    void start(Job job, const QString &program, const QStringList &arguments);
    void startNextThumbnail();

    QProcess process;
    QProcess thumbnailProcess;
    QTemporaryDir temporaryDir;
    Job job = Job::None;
    QString pdfSource;
    QString renderPrefix;
    int requestedPage = 1;
    bool cancellationRequested = false;
    int nextThumbnail = 0;
    int thumbnailCount = 0;
    QPoint recognitionOffset;
    QRect recognitionRegion;
};

} // namespace yagf::processing
