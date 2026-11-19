#include "experimentrunner.h"
#include "projectfile.h"
#include <QGuiApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <csignal>
#include <thread>
#include <chrono>
#include <cstdio>

namespace { volatile std::sig_atomic_t interrupted=0; void interrupt(int){interrupted=1;} }
int main(int argc,char **argv)
{
    if(qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))qputenv("QT_QPA_PLATFORM","offscreen");
    QGuiApplication app(argc,argv);app.setApplicationName("yagf2-benchmark");app.setApplicationVersion(QStringLiteral(YAGF_VERSION));
    QCommandLineParser parser;parser.setApplicationDescription("Compare OCR pipelines against disjoint verified reference pages without changing a project.");parser.addHelpOption();parser.addVersionOption();
    parser.addPositionalArgument("project","YAGF project containing verified reference text.");
    parser.addOption({"pipelines","JSON file containing a pipelines array.","file"});parser.addOption({"calibration","Comma-separated calibration page numbers.","pages"});parser.addOption({"held-out","Comma-separated held-out page numbers.","pages"});parser.addOption({"output","Destination experiment JSON report.","file"});
    parser.addOption({"repetitions","Measured repetitions per page and pipeline.","count","2"});parser.addOption({"warmups","Calibration warmups per pipeline.","count","1"});
    parser.addOption({"hardware-report","Optional separately collected hardware observation JSON.","file"});
    parser.process(app);
    const auto fail=[](const QString &message){std::fprintf(stderr,"%s\n",qPrintable(message));return 2;};
    if(parser.positionalArguments().size()!=1 || !parser.isSet("pipelines") || !parser.isSet("output"))return fail("Provide a project, --pipelines, --calibration, --held-out, and --output.");
    yagf::core::OcrDocument document;QString error;
    if(!yagf::core::ProjectFile::load(parser.positionalArguments()[0],&document,&error))return fail(error);
    QFile config(parser.value("pipelines"));if(!config.open(QIODevice::ReadOnly) || config.size()>1024*1024)return fail("Cannot read bounded pipeline configuration.");
    const auto json=QJsonDocument::fromJson(config.readAll());if(!json.isObject() || !json.object()["pipelines"].isArray())return fail("Expected a pipelines array.");
    yagf::experiments::ExperimentPlan plan;bool countOk=false,warmOk=false;plan.repetitions=parser.value("repetitions").toInt(&countOk);plan.warmups=parser.value("warmups").toInt(&warmOk);if(!countOk || !warmOk)return fail("Invalid repetition/warmup count.");
    for(const auto &value:json.object()["pipelines"].toArray()) {
        if(!value.isObject())return fail("Invalid pipeline entry.");const auto p=value.toObject();
        if(p.contains("token"))return fail("Use token_env for runtime credentials; do not store tokens in pipeline files.");
        yagf::experiments::Pipeline pipeline;pipeline.id=p["id"].toString();pipeline.label=p["label"].toString(pipeline.id);pipeline.provider=p["provider"].toString("tesseract");pipeline.endpoint=QUrl(p["endpoint"].toString());pipeline.language=p["language"].toString("eng");
        if(p.contains("dpi") && (!p["dpi"].isDouble() || p["dpi"].toDouble()!=p["dpi"].toInt()))return fail("DPI must be an integer.");
        pipeline.dpi=p["dpi"].toInt();for(const auto *field:{"binarize","auto_deskew","saved_filters","contrast","denoise"})if(p.contains(QLatin1String(field)) && !p[QLatin1String(field)].isBool())return fail("Preprocessing flags must be booleans.");
        pipeline.binarize=p["binarize"].toBool();pipeline.autoDeskew=p["auto_deskew"].toBool();pipeline.savedFilters=p["saved_filters"].toBool(true);pipeline.contrast=p["contrast"].toBool();pipeline.denoise=p["denoise"].toBool();
        const QString name=p["token_env"].toString();if(!name.isEmpty()){pipeline.token=qEnvironmentVariable(name.toUtf8().constData());if(pipeline.token.isEmpty())return fail("A configured token environment variable is unset.");}
        plan.pipelines.append(pipeline);
    }
    for(const auto &option:{QStringLiteral("calibration"),QStringLiteral("held-out")})for(const auto &text:parser.value(option).split(',',Qt::SkipEmptyParts)) {
        bool ok=false;const int number=text.trimmed().toInt(&ok);if(!ok || number<1)return fail("Invalid reference page number.");plan.references.append({number,option=="held-out"});
    }
    std::signal(SIGINT,interrupt);std::stop_source stop;
    std::jthread monitor([&](std::stop_token done){while(!done.stop_requested()){if(interrupted){stop.request_stop();return;}std::this_thread::sleep_for(std::chrono::milliseconds(25));}});
    auto report=yagf::experiments::runExperiment(document,plan,stop.get_token(),[](int n,int total,const QString &message){std::fprintf(stderr,"%d/%d %s\n",n,total,qPrintable(message));});monitor.request_stop();
    if(!report){if(interrupted)return 130;return fail(report.error());}bool hardwareFailed=false;
    if(parser.isSet("hardware-report")) {
        QFile file(parser.value("hardware-report"));
        if(!file.open(QIODevice::ReadOnly) || file.size()>1024*1024){(*report)["hardware_observation_error"]="Hardware report is unavailable at benchmark completion";hardwareFailed=true;}
        else {
            const auto data=QJsonDocument::fromJson(file.readAll());
            if(!data.isObject() || data.object()["schema"]!="yagf-hardware/1"){(*report)["hardware_observation_error"]="Incompatible hardware report";hardwareFailed=true;}
            else {
                (*report)["hardware_observation"]=data.object();
                const auto start=QDateTime::fromString(report->value("started_utc").toString(),Qt::ISODateWithMs),end=QDateTime::fromString(report->value("finished_utc").toString(),Qt::ISODateWithMs);
                const auto from=QDateTime::fromString(data.object()["started_utc"].toString(),Qt::ISODateWithMs),to=QDateTime::fromString(data.object()["finished_utc"].toString(),Qt::ISODateWithMs);
                if(start.isValid() && end.isValid() && from.isValid() && to.isValid()) {
                    const auto overlap=qMax<qint64>(0,qMin(end,to).toMSecsSinceEpoch()-qMax(start,from).toMSecsSinceEpoch());(*report)["hardware_overlap_ms"]=overlap;
                    if(!overlap){(*report)["hardware_observation_error"]="Hardware sampling did not overlap inference";hardwareFailed=true;}
                } else {(*report)["hardware_observation_error"]="Hardware observation timestamps are unavailable";hardwareFailed=true;}
            }
        }
    }
    QSaveFile output(parser.value("output"));const auto bytes=QJsonDocument(*report).toJson();if(!output.open(QIODevice::WriteOnly) || output.write(bytes)!=bytes.size() || !output.commit())return fail(output.errorString());
    std::printf("%s\n",QJsonDocument(report->value("summary").toArray()).toJson().constData());
    if(report->value("cancelled").toBool())return 130;
    for(const auto &row:report->value("summary").toArray())if(!row.toObject()["complete"].toBool())return 1;
    return hardwareFailed?1:0;
}
