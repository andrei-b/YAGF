#pragma once
#include "iocrengine.h"
#include "modelprofile.h"
#include <QUrl>
#include <memory>

namespace yagf::processing {
struct OcrService {
    QString provider, label, language, token, error;
    QUrl url;
    bool enabled=false, ready=false;
    QString model;
    QStringList tasks;
    std::optional<ModelProfile> profile;
    QString protocol; // Empty in legacy settings; kept separate from capabilities.
    QString key() const;
    bool supportsFormulas() const;
    bool supportsText() const;
    bool supportsPageLayout() const;
    ModelProfile modelProfile() const;
};
// Prefer an existing user choice when it matches. Never select an unready model.
std::optional<OcrService> selectOcrModel(const QVector<OcrService> &services,
    const ModelRequirements &requirements, const QString &preferredKey = {});
OcrService localFormulaService();
bool localOcrProvider(const QString &provider);
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
