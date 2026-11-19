#pragma once
#include "ocrbenchmark.h"
#include "iocrengine.h"
#include <QUrl>
#include <functional>
#include <memory>
namespace yagf::experiments {
struct Pipeline {
    QString id, label;
    QString provider = QStringLiteral("tesseract");
    QUrl endpoint;
    QString token; // Runtime only; never serialized in a report.
    QString language = QStringLiteral("eng");
    int dpi = 0; // 0 saved page DPI, -1 adaptive, otherwise a supported fixed DPI.
    bool binarize = false;
    bool savedFilters = true;
    bool contrast = false;
    bool denoise = false;
    bool autoDeskew = false;
    QJsonObject toJson() const;
};
struct ReferencePage { int number = 1; bool heldOut = false; };
struct ExperimentPlan {
    QVector<Pipeline> pipelines;
    QVector<ReferencePage> references;
    int repetitions = 2;
    int warmups = 1;
};
using EngineFactory = std::function<std::unique_ptr<processing::IOcrEngine>(const Pipeline &)>;
using Progress = std::function<void(int completed,int total,const QString &message)>;
std::unique_ptr<processing::IOcrEngine> makeEngine(const Pipeline &pipeline);
std::expected<QJsonObject,QString> runExperiment(const core::OcrDocument &document,const ExperimentPlan &plan,
    std::stop_token stop = {},const Progress &progress = {},const EngineFactory &factory = {});
QJsonArray summarizeExperiment(const QJsonObject &report);
}
