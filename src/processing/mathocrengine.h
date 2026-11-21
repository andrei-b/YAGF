#pragma once

#include "iocrengine.h"

namespace yagf::processing {

class MathOcrEngine final : public IOcrEngine {
public:
    QString id() const override { return QStringLiteral("rapid-latex-ocr"); }
    EngineCapabilities capabilities() const override
    { return {.supportsRegions = true, .supportsLayout = true, .supportsFormulas = true}; }
    static QString pythonPath();
    // With warnings, individual failed crops are omitted so the caller can
    // retain native text there. Without warnings, any crop failure is an error.
    std::expected<core::OcrPage, OcrError> recognizeBoxes(
        const QImage &image, const OcrOptions &options, std::stop_token stop, const QList<QRect> &boxes,
        QStringList *warnings = nullptr, int timeBudgetMs = 300000);
    std::expected<RecognitionResult, OcrError> recognizeResult(
        const QImage &image, const OcrOptions &options, std::stop_token stop) override;
};

// Uses the selected remote recognizer, or batches through the local model when null.
std::expected<core::OcrPage, OcrError> recognizeFormulaBoxes(
    IOcrEngine *provider, const QImage &image, const OcrOptions &options, std::stop_token stop,
    const QList<QRect> &boxes, QStringList *warnings = nullptr, int timeBudgetMs = 30000);

} // namespace yagf::processing
