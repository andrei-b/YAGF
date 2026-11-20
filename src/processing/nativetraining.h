#pragma once
#include "iocrengine.h"
#include <QUrl>

namespace yagf::processing {
struct NativeTrainingSample {
    QString id;
    int page=1;
    QRect bounds;
    QString text;
    QByteArray png;
    QString decision=QStringLiteral("pending"); // approved, pending, excluded
    QString partition=QStringLiteral("train"); // train or validation
    bool previouslyReviewed=false;
};
QJsonObject trainingSampleJson(const NativeTrainingSample &sample);
std::expected<QVector<NativeTrainingSample>, QString> readTrainingSamples(const QJsonValue &value);
std::expected<QJsonObject,OcrError> nativeTrainingRequest(const QUrl &base,const QString &token,
    const QByteArray &method,const QString &path,const QJsonObject &body={},std::stop_token stop={});
}
