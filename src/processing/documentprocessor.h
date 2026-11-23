#pragma once

#include <QObject>
#include <QHash>
#include <QProcess>
#include <QFutureWatcher>
#include <QRect>
#include <QPoint>
#include <QTemporaryDir>
#include <QVector>
#include <QSize>

#include "preprocesssettings.h"
#include "bookvocabulary.h"
#include "iocrengine.h"
#include "parallelocr.h"
#include "aipagelayout.h"

#include <memory>
#include <optional>
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
    explicit DocumentProcessor(QObject *parent = nullptr, const core::OcrDocument *book = nullptr);
    ~DocumentProcessor() override;
    bool isBusy() const;
    bool isRecognizing() const;
    QString engineId() const;
    EngineCapabilities engineCapabilities() const;
    OcrService primaryOcrService() const;
    void setComparisonEngines(std::vector<ComparisonEngine> engines);
    std::expected<void, QString> setFormulaOcrService(OcrService service);
    OcrService formulaOcrService() const;
    std::expected<void, QString> setPageLayoutService(PageLayoutOptions options,bool enabled);
    void setEngine(std::unique_ptr<IOcrEngine> replacement);
    void setNativeEngine(std::unique_ptr<IOcrEngine> replacement);
    void setMathMode(bool enabled);
    void setSpecialists(std::unique_ptr<IOcrEngine> formulas, std::unique_ptr<IOcrEngine> prose);
    void resetSource();
    void openDocument(const QString &path);
    void renderDocumentPage(int pageNumber, int dpiOverride = 0);
    int renderedDpi(int pageNumber) const;
    void startThumbnails(int pageCount);
    void splitBlocks(const QString &imagePath, const QString &language, int pageNumber);
    void recognize(const QString &imagePath, const QString &language, int pageNumber);
    void recognizeTypedRegions(const QString &imagePath, const QString &language, int pageNumber,
                               const QVector<core::ImageRegion> &selected);
    void recognizeRegion(const QString &imagePath, const QString &language,
                         int pageNumber, const QRect &region);
    void recognizePages(const QVector<BatchPageOptions> &pages);
    void cancel();
signals:
    void busyChanged(bool busy);
    void documentOpened(int pageCount);
    void pageRendered(int pageNumber, const QString &imagePath);
    void thumbnailReady(int pageNumber, const QString &imagePath);
    void blocksSplit(core::OcrPage page);
    void recognized(core::OcrPage page, const QPoint &offset, const QRect &region);
    void recognitionHistoryReady(core::OcrPage page);
    void batchPageRecognized(core::OcrPage page, const QSize &sourceSize);
    void batchProgress(qsizetype completed, qsizetype total);
    void batchFinished();
    void cancelled();
    void failed(const QString &message);
    void specialistWarning(const QString &message);
    void recognitionStage(int page, const QString &stage, int complete, int total);
private:
    enum class Job : quint8 { None, DocumentInfo, DocumentRender, Ocr, SplitBlocks, BatchRender, BatchOcr };
    void discardRenderedPage(Job finishedJob);
    int selectedRenderDpi(int number) const;
    QString pagePrefix(int number) const;
    QString pageFile(int number) const;
    QString sourcePath(int number) const;
    int sourcePage(int number) const;
    void selectRenderDpi(int number, int dpiOverride = 0);
    bool adaptRenderedPage(int number);
    void start(Job job, const QString &program, const QStringList &arguments);
    void startNextThumbnail();
    void startNextBatchPage();
    void prepareBatchImage(const QString &path);
    void finishBatchError(const QString &message);
    void startOcrImage(const QString &path, const QImage &image, OcrOptions options,
                       const QRect &region, bool batch, bool splitOnly = false, QVector<core::ImageRegion> selected = {});

    struct OcrJobResult {
        std::expected<core::OcrPage, OcrError> outcome;
        core::OcrPage historyPage;
        QStringList warnings;
        QPoint offset;
        QRect region;
        QVector<core::ImageRegion> selected;
    };

    const core::OcrDocument *book; // Read only on the owning thread, before starting an OCR job.
    core::BookVocabulary batchVocabulary;
    QString vocabularyLanguage;
    int nextEvidencePage = 1;
    QProcess process;
    QProcess thumbnailProcess;
    QFutureWatcher<OcrJobResult> ocrWatcher;
    std::stop_source ocrStop;
    std::unique_ptr<IOcrEngine> engine;
    std::unique_ptr<IOcrEngine> nativePreviousEngine;
    std::unique_ptr<IOcrEngine> textEngine; // Restored when leaving local math mode.
    std::unique_ptr<IOcrEngine> formulaEngine, proseEngine;
    std::unique_ptr<IOcrEngine> formulaRecognizer;
    OcrService selectedFormulaService;
    std::optional<PageLayoutOptions> pageLayoutService;
    std::vector<ComparisonEngine> comparisonEngines;
    QTemporaryDir temporaryDir;
    Job job = Job::None;
    QString documentSource;
    QString renderPrefix;
    int requestedPage = 1;
    bool cancellationRequested = false;
    int nextThumbnail = 0;
    int thumbnailCount = 0;
    QVector<BatchPageOptions> batchPages;
    qsizetype batchIndex = 0;
    QSize batchSourceSize;
    core::CoordinatePipeline batchCoordinates;
    QHash<int, int> renderedDpis;
    QHash<int, int> readyDpis;
    bool adaptivePilot = false;
};

} // namespace yagf::processing
