    #pragma once

#include "ocrdocument.h"

#include <QImage>

#include <expected>
#include <stop_token>

namespace yagf::processing {

struct EngineCapabilities {
    bool supportsRegions = false;
    bool supportsWordBoxes = false;
    bool supportsConfidence = false;
    bool supportsLayout = false;
    bool supportsTables = false;
};

struct OcrOptions {
    QString language;
    int pageNumber = 1;
    QStringList userWords;
    bool preserveFormatting = false;
    bool collectBenchmarkMetrics = false;
    bool singleTextLine = false;
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
    virtual std::expected<core::OcrPage, OcrError> recognize(
        const QImage &image, const OcrOptions &options, std::stop_token stop) = 0;
};

} // namespace yagf::processing
