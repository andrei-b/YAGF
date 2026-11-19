#pragma once

#include "iocrengine.h"

#include <QUrl>

namespace yagf::processing {

// A synchronous OCR provider. DocumentProcessor runs it on a worker thread.
class RemoteOcrEngine final : public IOcrEngine {
public:
    enum class Provider : quint8 { PaddleOcr, DocTr, RapidOcr, Surya, Kraken };
    RemoteOcrEngine(QUrl server, QString bearerToken = {}, Provider provider = Provider::PaddleOcr);
    [[nodiscard]] QString id() const override;
    [[nodiscard]] EngineCapabilities capabilities() const override;
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
};

} // namespace yagf::processing
