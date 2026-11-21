#pragma once
#include <QJsonObject>
#include <QSize>
#include <QStringList>
#include <expected>
#include <optional>

namespace yagf::processing {
// Missing metadata means unknown, never unlimited or unsupported. "ocr" is a
// legacy wire-task alias for the canonical "text" capability.
struct ModelRequirements {
    QStringList tasks;
    QString geometry, format, language;
    QSize imageSize;
    bool batching = false;
};
struct ModelProfile {
    QString model;
    QStringList tasks;
    QString geometry = "unknown";
    QStringList formats, languages;
    qint64 maxImagePixels = 0;
    std::optional<bool> supportsBatching;
    QStringList incompatibilities(const ModelRequirements &requirements) const;
};
QString canonicalModelTask(const QString &task);
QJsonObject modelProfileJson(const ModelProfile &profile);
std::expected<ModelProfile, QString> readModelProfile(const QJsonValue &json);
}
