#pragma once

#include "ocrdocument.h"
#include "recognitionadapter.h"

#include <QImage>

#include <expected>
#include <functional>
#include <stop_token>

namespace yagf::processing {
using core::RecognitionResult;
using OcrProgress = std::function<void(const QString &, int, int)>;

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
    virtual std::expected<RecognitionResult, OcrError> recognizeResult(
        const QImage &image, const OcrOptions &options, std::stop_token stop) = 0;
    // Compatibility entry point for editor-oriented consumers. Engine
    // implementations only supply native evidence via recognizeResult().
    std::expected<core::OcrPage, OcrError> recognize(
        const QImage &image, const OcrOptions &options, std::stop_token stop) {
        auto result = recognizeResult(image, options, stop);
        if (!result) return std::unexpected(result.error());
        if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
        if (result->engineId.isEmpty()) result->engineId = id();
        auto page = recognitionToPage(*result);
        if (!page) return std::unexpected(OcrError{page.error()});
        return std::move(*page);
    }
};

} // namespace yagf::processing
