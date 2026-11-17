#pragma once

#include <QObject>
#include <QProcess>
#include <QRect>
#include <QPoint>
#include <QTemporaryDir>
#include <QVector>
#include <QSize>

#include "preprocesssettings.h"

namespace yagf::processing {

struct BatchPageOptions {
    int number = 1;
    QString language;
    core::PreprocessSettings preprocessing;
};

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
    void recognizePages(const QVector<BatchPageOptions> &pages);
    void cancel();
signals:
    void busyChanged(bool busy);
    void pdfOpened(int pageCount);
    void pageRendered(int pageNumber, const QString &imagePath);
    void thumbnailReady(int pageNumber, const QString &imagePath);
    void recognized(const QByteArray &tsv, const QPoint &offset, const QRect &region);
    void batchPageRecognized(int pageNumber, const QByteArray &tsv, const QSize &sourceSize,
                             const QSize &processedSize, const QString &language);
    void batchProgress(int completed, int total);
    void batchFinished();
    void cancelled();
    void failed(const QString &message);
private:
    enum class Job { None, PdfInfo, PdfRender, Ocr, BatchRender, BatchOcr };
    void start(Job job, const QString &program, const QStringList &arguments);
    void startNextThumbnail();
    void startNextBatchPage();
    void prepareBatchImage(const QString &path);
    void finishBatchError(const QString &message);

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
    QVector<BatchPageOptions> batchPages;
    int batchIndex = 0;
    QSize batchSourceSize;
    QSize batchProcessedSize;
};

} // namespace yagf::processing
