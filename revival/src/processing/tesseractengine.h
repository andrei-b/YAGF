#pragma once

#include "iocrengine.h"

namespace yagf::processing {

class TesseractEngine final : public IOcrEngine {
public:
    QString id() const override { return QStringLiteral("tesseract"); }
    EngineCapabilities capabilities() const override
    { return {.supportsRegions = true, .supportsWordBoxes = true,
              .supportsConfidence = true, .supportsLayout = true}; }
    std::expected<core::OcrPage, OcrError> recognize(
        const QImage &image, const OcrOptions &options, std::stop_token stop) override;

    static std::expected<core::OcrPage, OcrError> parseTsv(
        const QByteArray &tsv, const QSize &imageSize, const OcrOptions &options);
};

} // namespace yagf::processing
