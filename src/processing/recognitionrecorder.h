#pragma once
#include "iocrengine.h"
#include <QElapsedTimer>
#include <mutex>

namespace yagf::processing {
class RecognitionRecorder {
    mutable std::mutex mutex;
    core::RecognitionHistory history;
    QHash<QString,QByteArray> images;
public:
    const QString session, sourceHash;
    const QJsonObject preprocessing;
    RecognitionRecorder(QString sourceHash,QJsonObject preprocessing);
    core::RecognitionRun begin(const QString &engine,const QString &model,const QImage &image,const OcrOptions &options);
    core::RecognitionRun finish(core::RecognitionRun run,qint64 elapsed,const QString &status,const QString &error,
        QJsonObject output={},QJsonObject runtime={},QStringList warnings={});
    core::RecognitionHistory snapshot() const;
};
QString recognitionSourceHash(const QString &path,std::stop_token stop);
}
