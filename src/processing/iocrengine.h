#pragma once

#include "ocrdocument.h"
#include "recognitionadapter.h"

#include <QImage>

#include <expected>
#include <functional>
#include <stop_token>
#include <memory>

namespace yagf::processing {
using core::RecognitionResult;
using OcrProgress = std::function<void(const QString &, int, int)>;
class RecognitionRecorder;

struct EngineCapabilities {
    bool supportsRegions = false;
    bool supportsWordBoxes = false;
    bool supportsConfidence = false;
    bool supportsLayout = false;
    bool supportsTables = false;
    bool supportsFormulas = false;
};

struct OcrOptions {
    QString language;
    int pageNumber = 1;
    QStringList userWords;
    bool preserveFormatting = false;
    bool collectBenchmarkMetrics = false;
    bool singleTextLine = false;
    OcrProgress progress;
    bool formula = false;
    int timeoutMs = 180000;
    // Optional negotiated task/format. Empty retains the existing OCR/formula call.
    QString task;
    QString outputFormat;
    std::shared_ptr<RecognitionRecorder> history;
    QString historyRole="primary", historySource;
    QPoint historyOffset;
    core::CoordinatePipeline historyCoordinates;
    bool historyNativeCall=false;
};

struct OcrError {
    QString message;
    bool cancelled = false;
};

class IOcrEngine {
public:
    virtual ~IOcrEngine() = default;
    [[nodiscard]] virtual QString id() const = 0;
    [[nodiscard]] virtual EngineCapabilities capabilities() const = 0;
    [[nodiscard]] virtual QJsonObject lastRunDetails() const { return {}; }
    [[nodiscard]] virtual QJsonObject lastStructuredOutput() const { return {}; }
    [[nodiscard]] virtual QString modelIdentifier() const { return id(); }
    virtual std::expected<RecognitionResult, OcrError> recognizeResult(
        const QImage &image, const OcrOptions &options, std::stop_token stop) = 0;
    // Compatibility entry point for editor-oriented consumers. Engine
    // implementations only supply native evidence via recognizeResult().
    std::expected<core::OcrPage, OcrError> recognize(
        const QImage &image, const OcrOptions &options, std::stop_token stop);
};

OcrOptions croppedOcrOptions(const OcrOptions &options,QRect crop,int padding=0);

} // namespace yagf::processing
