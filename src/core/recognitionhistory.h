#pragma once
#include "recognitionresult.h"
#include "preprocesssettings.h"
#include <QByteArray>
#include <QStringList>

namespace yagf::core {
class CoordinateFrames;
struct RecognitionRun {
    QString id, session, role, source, engine, model, version, modelHash;
    QString sourceHash, inputHash, preprocessingHash, startedUtc, status, error;
    qint64 elapsedMs=0;
    QSize inputSize;
    QRect processedCrop;
    QPolygonF sourceCrop;
    CoordinatePipeline sourceToInput;
    QJsonObject preprocessing, output, runtime;
    QByteArray inputPng;
    QStringList warnings;
};
struct RecognitionDecision {
    QString id, runId, decision, timestampUtc, proposedText, primaryText;
    QStringList wordIds;
    QRect processedArea;
    CoordinatePipeline sourceToProcessed;
};
struct RecognitionHistory {
    QVector<RecognitionRun> runs;
    QVector<RecognitionDecision> decisions;
};
QString sha256(const QByteArray &bytes);
QJsonObject preprocessingEvidence(const PreprocessSettings &settings,int dpi);
QJsonObject recognitionHistoryJson(const RecognitionHistory &history,CoordinateFrames *frames=nullptr);
std::expected<RecognitionHistory,QString> readRecognitionHistory(const QJsonValue &json,const CoordinateFrames *frames=nullptr);
std::expected<void,QString> validateRecognitionHistory(const RecognitionHistory &history);
void mergeRecognitionHistory(RecognitionHistory &target,const RecognitionHistory &incoming);
}
