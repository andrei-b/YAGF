#pragma once

#include "iocrengine.h"

#include <QUrl>

namespace yagf::processing {

// A synchronous OCR provider. DocumentProcessor runs it on a worker thread.
class RemoteOcrEngine final : public IOcrEngine {
public:
    RemoteOcrEngine(QUrl server, QString bearerToken = {});
    QString id() const override { return QStringLiteral("paddleocr-remote"); }
    EngineCapabilities capabilities() const override
    { return {.supportsRegions = true, .supportsConfidence = true}; }
    std::expected<core::OcrPage, OcrError> recognize(
        const QImage &image, const OcrOptions &options, std::stop_token stop) override;

private:
    QUrl server;
    QString bearerToken;
};

} // namespace yagf::processing
