#pragma once

#include "iocrengine.h"

#include <QUrl>

namespace yagf::processing {

// A synchronous OCR provider. DocumentProcessor runs it on a worker thread.
class RemoteOcrEngine final : public IOcrEngine {
public:
    enum class Provider { PaddleOcr, DocTr, RapidOcr, Surya, Kraken };
    RemoteOcrEngine(QUrl server, QString bearerToken = {}, Provider provider = Provider::PaddleOcr);
    QString id() const override;
    EngineCapabilities capabilities() const override;
    std::expected<core::OcrPage, OcrError> recognize(
        const QImage &image, const OcrOptions &options, std::stop_token stop) override;

private:
    QUrl server;
    QString bearerToken;
    Provider provider;
};

} // namespace yagf::processing
