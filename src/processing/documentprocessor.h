#pragma once

#include <QObject>
#include <QProcess>
#include <QFutureWatcher>
#include <QRect>
#include <QPoint>
#include <QTemporaryDir>
#include <QVector>
#include <QSize>

#include "preprocesssettings.h"
#include "iocrengine.h"

#include <memory>
#include <stop_token>

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
    QString engineId() const;
    EngineCapabilities engineCapabilities() const;
    void setEngine(std::unique_ptr<IOcrEngine> replacement);
    void resetSource();
    void openPdf(const QString &path);
    void renderPdfPage(int pageNumber);
    void startThumbnails(int pageCount);
    void recognize(const QString &imagePath, const QString &language, int pageNumber);
    void recognizeRegion(const QString &imagePath, const QString &language,
                         int pageNumber, const QRect &region);
    void recognizePages(const QVector<BatchPageOptions> &pages);
    void cancel();
signals:
    void busyChanged(bool busy);
    void pdfOpened(int pageCount);
    void pageRendered(int pageNumber, const QString &imagePath);
    void thumbnailReady(int pageNumber, const QString &imagePath);
    void recognized(core::OcrPage page, const QPoint &offset, const QRect &region);
    void batchPageRecognized(core::OcrPage page, const QSize &sourceSize);
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
    void startOcrImage(const QString &path, const QImage &image, const OcrOptions &options,
                       const QRect &region, bool batch);

    struct OcrJobResult {
        std::expected<core::OcrPage, OcrError> outcome;
        QPoint offset;
        QRect region;
    };

    QProcess process;
    QProcess thumbnailProcess;
    QFutureWatcher<OcrJobResult> ocrWatcher;
    std::stop_source ocrStop;
    std::unique_ptr<IOcrEngine> engine;
    QTemporaryDir temporaryDir;
    Job job = Job::None;
    QString pdfSource;
    QString renderPrefix;
    int requestedPage = 1;
    bool cancellationRequested = false;
    int nextThumbnail = 0;
    int thumbnailCount = 0;
    QVector<BatchPageOptions> batchPages;
    int batchIndex = 0;
    QSize batchSourceSize;
};

} // namespace yagf::processing
