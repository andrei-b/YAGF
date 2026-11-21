#pragma once

#include "iocrengine.h"
#include "modelprofile.h"

#include <QUrl>

namespace yagf::processing {

// A synchronous OCR provider. DocumentProcessor runs it on a worker thread.
class RemoteOcrEngine final : public IOcrEngine {
public:
    enum class Provider : quint8 { PaddleOcr, DocTr, RapidOcr, Surya, Kraken, Native, PaddleVl, Generic };
    RemoteOcrEngine(QUrl server, QString bearerToken = {}, Provider provider = Provider::PaddleOcr,
                    QString model = {}, bool formulas = false,
                    std::optional<ModelProfile> profile = {}, QString engineId = {});
    [[nodiscard]] QString id() const override;
    [[nodiscard]] EngineCapabilities capabilities() const override;
    QUrl serviceUrl() const { return server; }
    QString sessionToken() const { return bearerToken; }
    QString modelName() const { return model; }
    QString serviceProtocol() const { return provider==Provider::PaddleVl?QStringLiteral("openai-chat/1"):QStringLiteral("yagf-ocr/1"); }
    std::optional<ModelProfile> modelProfile() const { return profile; }
    std::expected<RecognitionResult, OcrError> recognizeResult(
        const QImage &image, const OcrOptions &options, std::stop_token stop) override;

    QJsonObject lastRunDetails() const override { return runDetails; }
private:
    QJsonObject runDetails;
    std::expected<RecognitionResult, OcrError> recognizeSection(
        const QImage &image, const OcrOptions &options, std::stop_token stop, int depth);
    QUrl server;
    QString bearerToken;
    Provider provider;
    QString model;
    bool formulas = false;
    std::optional<ModelProfile> profile;
    QString engineName;
};

} // namespace yagf::processing
