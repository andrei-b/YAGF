#pragma once

#include "iocrengine.h"

namespace yagf::processing {

class MathOcrEngine final : public IOcrEngine {
public:
    QString id() const override { return QStringLiteral("rapid-latex-ocr"); }
    EngineCapabilities capabilities() const override
    { return {.supportsRegions = true, .supportsLayout = true}; }
    static QString pythonPath();
    std::expected<core::OcrPage, OcrError> recognize(
        const QImage &image, const OcrOptions &options, std::stop_token stop) override;
};

} // namespace yagf::processing
