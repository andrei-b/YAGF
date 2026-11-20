#pragma once
#include "iocrengine.h"
#include <QUrl>
#include <memory>

namespace yagf::processing {
struct OcrService {
    QString provider, label, language, token, error;
    QUrl url;
    bool enabled=false, ready=false;
    QString key() const;
};
QStringList ocrProviders();
QString ocrProviderLabel(const QString &provider);
bool validOcrServiceUrl(const QUrl &url);
QJsonArray ocrServicesJson(const QVector<OcrService> &services);
QVector<OcrService> readOcrServices(const QJsonValue &json);
QVector<QUrl> ocrDiscoveryTargets(const QString &hosts,const QVector<OcrService> &known);
struct OcrDiscovery {QVector<OcrService> services; QStringList diagnostics;};
std::expected<OcrDiscovery,OcrError> discoverOcrServices(const QVector<QUrl> &targets,
    const QVector<OcrService> &known,std::stop_token stop);
std::expected<std::unique_ptr<IOcrEngine>,QString> makeOcrServiceEngine(const OcrService &service);
}
