#pragma once

#include "iocrengine.h"
#include <atomic>

namespace yagf::processing {

class TesseractEngine final : public IOcrEngine {
public:
    [[nodiscard]] QString id() const override { return QStringLiteral("tesseract"); }
    [[nodiscard]] EngineCapabilities capabilities() const override
    { return {.supportsRegions = true, .supportsWordBoxes = true,
              .supportsConfidence = true, .supportsLayout = true}; }
    using IOcrEngine::recognize;
    std::expected<RecognitionResult, OcrError> recognizeResult(
        const QImage &image, const OcrOptions &options, std::stop_token stop) override;
    std::expected<core::OcrPage, OcrError> recognize(
        const QImage &image, const OcrOptions &options, std::stop_token stop,
        const std::atomic_bool *cancelled);

    static QString modelLanguage(const QString &language);
    QJsonObject lastStructuredOutput() const override { return structuredOutput; }
    QJsonObject lastRunDetails() const override { return runDetails; }
    static std::expected<core::OcrPage, OcrError> parseTsv(
        const QByteArray &tsv, const QSize &imageSize, const OcrOptions &options);
private:
    QJsonObject runDetails,structuredOutput;
};

} // namespace yagf::processing
