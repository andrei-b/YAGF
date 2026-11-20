#pragma once

#include "iocrengine.h"

#include <QUrl>

namespace yagf::processing {

// A synchronous OCR provider. DocumentProcessor runs it on a worker thread.
class RemoteOcrEngine final : public IOcrEngine {
public:
    enum class Provider : quint8 { PaddleOcr, DocTr, RapidOcr, Surya, Kraken, Native, PaddleVl };
    RemoteOcrEngine(QUrl server, QString bearerToken = {}, Provider provider = Provider::PaddleOcr,
                    QString model = {}, bool formulas = false);
    [[nodiscard]] QString id() const override;
    [[nodiscard]] EngineCapabilities capabilities() const override;
    QUrl serviceUrl() const { return server; }
    QString sessionToken() const { return bearerToken; }
    QString modelName() const { return model; }
    std::expected<core::OcrPage, OcrError> recognize(
        const QImage &image, const OcrOptions &options, std::stop_token stop) override;

    QJsonObject lastRunDetails() const override { return runDetails; }
private:
    QJsonObject runDetails;
    std::expected<core::OcrPage, OcrError> recognizeSection(
        const QImage &image, const OcrOptions &options, std::stop_token stop, int depth);
    QUrl server;
    QString bearerToken;
    Provider provider;
    QString model;
    bool formulas = false;
};

} // namespace yagf::processing
