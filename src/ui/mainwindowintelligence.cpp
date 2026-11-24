#include "mainwindow.h"
#include "documentcommands.h"
#include "intelligencedialog.h"
#include "resultreviewdialog.h"
#include "ocrreadings.h"
#include "coordinateframes.h"
#include "parallelocr.h"
#include "recognitionrecorder.h"

#include <QLabel>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QtConcurrentRun>
#include <algorithm>

namespace yagf::ui {
namespace {
struct AdditionalReading {
    core::OcrReading reading;
    core::RecognitionHistory history;
};

std::expected<AdditionalReading, QString> requestReading(const processing::OcrService& service,
    const QImage& scan, QRect crop, processing::OcrOptions options, const QString& sourcePath,
    const QJsonObject& preprocessing, std::stop_token stop)
{
    auto engine = processing::makeOcrServiceEngine(service);
    if (!engine)
        return std::unexpected(engine.error());
    const auto sourceHash = processing::recognitionSourceHash(sourcePath, stop);
    if (stop.stop_requested())
        return std::unexpected(QStringLiteral("Cancelled"));
    options.history = std::make_shared<processing::RecognitionRecorder>(sourceHash, preprocessing);
    std::vector<processing::ComparisonEngine> engines;
    engines.push_back({ service, std::move(*engine) });
    processing::ParallelOcrRun run(engines, scan, options, { QRegion(crop) }, stop);
    auto readings = run.finish();
    if (readings.isEmpty())
        return std::unexpected(QStringLiteral("No OCR reading returned."));
    if (!readings.first().error.isEmpty())
        return std::unexpected(readings.first().error);
    if (readings.first().items.isEmpty())
        return std::unexpected(QStringLiteral("The model found no text in this crop."));
    return AdditionalReading { std::move(readings.first()), options.history->snapshot() };
}

// Append a second observation. Re-recognition of one crop must preserve other services and earlier readings.
std::expected<void, QString> appendReading(core::OcrPage& page, AdditionalReading extra)
{
    auto& reading = extra.reading;
    auto it = std::ranges::find(page.ocrReadings, reading.source, &core::OcrReading::source);
    if (it == page.ocrReadings.end())
        page.ocrReadings.append(std::move(reading));
    else {
        it->area |= reading.area;
        it->items += reading.items;
        it->historyIds += reading.historyIds;
        it->recognitionEvidence += reading.recognitionEvidence;
        it->error.clear();
    }
    core::CoordinateFrames frames;
    const auto encoded = core::ocrReadingsJson(page.ocrReadings, &frames);
    if (auto valid = core::readOcrReadings(encoded, { page.width, page.height }, &frames); !valid)
        return std::unexpected(
            QStringLiteral("The new reading exceeds the project's supported evidence limits."));
    core::mergeRecognitionHistory(page.recognitionHistory, extra.history);
    return core::validateRecognitionHistory(page.recognitionHistory);
}
}

void MainWindow::reviewResultWithAi(QVector<int> words)
{
    if (processor.isBusy() || projectLoading || exportBusy || !maybeApplyEditor())
        return;
    const auto* page = document.page(currentPage);
    if (!page || page->words.isEmpty())
        return;
    if (words.isEmpty())
        for (int i = 0; i < page->words.size(); ++i)
            words.append(i);
    ResultReviewDialog dialog(
        *page, std::move(words), displayedPageImage(), document.unicodeFormulas, aiReviewToken, this);
    const auto outcome = dialog.exec();
    aiReviewToken = dialog.sessionToken();
    if (outcome == QDialog::Accepted && dialog.edited)
        undoStack.push(new ReplacePageCommand(
            document, currentPage, std::move(*dialog.edited), tr("Apply AI-reviewed OCR corrections")));
}

void MainWindow::inspectIntelligence()
{
    if (processor.isBusy() || projectLoading || exportBusy || !maybeApplyEditor())
        return;
    const int number = currentPage;
    const auto scan = displayedPageImage();
    auto available = ocrServices;
    // The explicitly selected primary and formula services also participate even when
    // companion recognition was disabled for them.
    for (auto configured : { processor.primaryOcrService(), processor.formulaOcrService() }) {
        auto it = std::ranges::find_if(available, [&](const auto& s) { return s.key() == configured.key(); });
        if (it != available.end()) {
            it->enabled = true;
            continue;
        }
        if (configured.provider == "tesseract")
            configured.ready = !QStandardPaths::findExecutable("tesseract").isEmpty();
        configured.enabled = true;
        available.append(configured);
    }
    QString area = selectedRegionIds.size() == 1 ? *selectedRegionIds.cbegin() : QString {};
    int focus = selectedWords.isEmpty() ? -1 : selectedWords.first();
    while (number == currentPage) {
        const auto* page = document.page(number);
        if (!page)
            return;
        IntelligenceDialog review(document, number, scan, available, area, focus, this);
        const int response = review.exec();
        if (response == IntelligenceDialog::ReviewWithAi) {
            reviewResultWithAi(review.aiWords);
            focus=review.word;
            continue;
        }
        if (response != QDialog::Accepted)
            return;
        if (review.edited) {
            const auto label = page->reviewedRelations == review.edited->reviewedRelations
                ? tr("Review document reading")
                : tr("Review document relation");
            undoStack.push(new ReplacePageCommand(document, number, std::move(*review.edited), label));
            return;
        }
        if (!review.requestedService)
            return;
        const auto service = *review.requestedService;
        processing::OcrOptions options;
        options.pageNumber = number;
        options.task = review.assessment.task;
        options.language = page->language.isEmpty() ? document.language : page->language;
        options.formula = options.task == "formula";
        options.singleTextLine = !page->words[review.word].layoutBlock;
        options.historyCoordinates = page->coordinates;
        std::stop_source cancellation;
        QProgressDialog progress(
            tr("Requesting a reading from %1…").arg(service.label), tr("Cancel"), 0, 0, this);
        progress.setWindowModality(Qt::WindowModal);
        QFutureWatcher<std::expected<AdditionalReading, QString>> watcher;
        connect(&watcher, &QFutureWatcherBase::finished, &progress, &QDialog::accept);
        connect(&progress, &QProgressDialog::canceled, this, [&] { cancellation.request_stop(); });
        watcher.setFuture(QtConcurrent::run(requestReading, service, scan, review.assessment.requestCrop,
            options, document.sourceForPage(number).path,
            core::preprocessingEvidence(page->preprocessing, page->renderDpi), cancellation.get_token()));
        if (progress.exec() != QDialog::Accepted)
            cancellation.request_stop();
        watcher.waitForFinished();
        if (cancellation.stop_requested())
            return;
        auto result = watcher.result();
        if (!result) {
            showError(result.error());
            return;
        }
        auto edited = *page;
        if (auto ok = appendReading(edited, std::move(*result)); !ok) {
            showError(ok.error());
            return;
        }
        undoStack.push(new ReplacePageCommand(
            document, number, std::move(edited), tr("Collect document recognition evidence")));
        focus = review.word;
        area.clear();
    }
}
}
