#include "experimentrunner.h"
#include "correctionretention.h"
#include "exportpage.h"
#include "imageprocessor.h"
#include "pageorientation.h"
#include "pagesource.h"
#include <QFileInfo>
#include <vector>
#include <limits>
#include "remoteocrengine.h"
#include "tesseractengine.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <QSysInfo>
#include <QThread>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <tuple>

namespace yagf::experiments {
namespace {
QString digest(const QByteArray &bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QJsonObject preprocessing(const core::PreprocessSettings &settings) {
    return {{"quarter_turns",settings.quarterTurns},{"deskew_degrees",settings.deskewDegrees},
        {"contrast",settings.autoContrast},{"denoise",settings.denoise},{"binarize",settings.adaptiveBinarization},{"ink_threshold",settings.inkThreshold}};
}
QJsonObject localHost() {
    return {{"os",QSysInfo::prettyProductName()},{"architecture",QSysInfo::currentCpuArchitecture()},
        {"kernel",QSysInfo::kernelVersion()},{"logical_cpus",QThread::idealThreadCount()},{"qt",qVersion()}};
}
QJsonObject engineVersion(const Pipeline &pipeline) {
    if(pipeline.provider!="tesseract")return {};
    QProcess process;process.start("tesseract",{"--version"});
    if(!process.waitForFinished(5000)){process.kill();process.waitForFinished(1000);return {{"engine_version","unavailable"}};}
    return {{"engine_version",QString::fromUtf8(process.readAllStandardOutput()).section('\n',0,0).left(256)}};
}
}
QJsonObject Pipeline::toJson() const {
    QUrl safe=endpoint;safe.setUserInfo({});safe.setQuery(QString());safe.setFragment({});
    return {{"id",id},{"label",label},{"provider",provider},{"endpoint",safe.toString()},
        {"language",language},{"dpi",dpi},{"binarize",binarize},{"auto_deskew",autoDeskew},{"saved_filters",savedFilters},{"contrast",contrast},{"denoise",denoise}};
}
std::unique_ptr<processing::IOcrEngine> makeEngine(const Pipeline &p) {
    using Engine=processing::RemoteOcrEngine;
    if(p.provider=="tesseract")return std::make_unique<processing::TesseractEngine>();
    const QHash<QString,Engine::Provider> providers{{"paddleocr",Engine::Provider::PaddleOcr},{"doctr",Engine::Provider::DocTr},
        {"rapidocr",Engine::Provider::RapidOcr},{"surya",Engine::Provider::Surya},{"kraken",Engine::Provider::Kraken}};
    if(!providers.contains(p.provider))return {};
    return std::make_unique<Engine>(p.endpoint,p.token,providers[p.provider]);
}
std::expected<QJsonObject,QString> runExperiment(const core::OcrDocument &document,const ExperimentPlan &plan,
    std::stop_token stop,const Progress &progress,const EngineFactory &factory)
{
    if(plan.pipelines.isEmpty() || plan.pipelines.size()>16 || plan.references.size()<2 || plan.references.size()>64 ||
        plan.repetitions<1 || plan.repetitions>10 || plan.warmups<0 || plan.warmups>3)
        return std::unexpected(QStringLiteral("Choose 1–16 pipelines, 2–64 reference pages, 1–10 repetitions, and 0–3 warmups."));
    auto referencesInOrder=plan.references;
    std::stable_sort(referencesInOrder.begin(),referencesInOrder.end(),[](const auto &a,const auto &b){return a.heldOut<b.heldOut;});
    if(qint64(plan.pipelines.size())*plan.references.size()*plan.repetitions>256)
        return std::unexpected(QStringLiteral("Limit one experiment report to 256 measured runs."));
    QSet<int> pages;bool calibration=false,heldOut=false;QSet<QString> ids;
    for(const auto &ref:referencesInOrder) {
        const auto *page=document.page(ref.number);
        if(!page || pages.contains(ref.number))return std::unexpected(QStringLiteral("Reference pages must exist and be distinct."));
        const auto problem=core::benchmarkReferenceProblem(*page);
        if(!problem.isEmpty())return std::unexpected(QStringLiteral("Page %1: %2").arg(ref.number).arg(problem));
        if(core::benchmarkText(*page).isEmpty())return std::unexpected(QStringLiteral("Reference text must not be empty."));
        pages.insert(ref.number);calibration|=!ref.heldOut;heldOut|=ref.heldOut;
    }
    if(!calibration || !heldOut)return std::unexpected(QStringLiteral("Keep at least one distinct page in each calibration and held-out set."));
    for(const auto &p:plan.pipelines) {
        if(p.id.isEmpty() || ids.contains(p.id) || (p.dpi!=0 && p.dpi!=-1 && !core::validRenderDpi(p.dpi)) || p.language.trimmed().isEmpty())
            return std::unexpected(QStringLiteral("Invalid or duplicate pipeline configuration."));
        if(p.provider!="tesseract" && (p.endpoint.host().isEmpty() || !p.endpoint.userInfo().isEmpty() ||
            (p.endpoint.scheme()!="http" && p.endpoint.scheme()!="https")))
            return std::unexpected(QStringLiteral("Remote pipelines require an HTTP(S) endpoint without embedded credentials."));
        ids.insert(p.id);
    }
    QFile source(document.sourcePath);if(!source.open(QIODevice::ReadOnly))return std::unexpected(source.errorString());
    QCryptographicHash sourceHash(QCryptographicHash::Sha256);
    while(!source.atEnd()){if(stop.stop_requested())return std::unexpected(QStringLiteral("Cancelled"));sourceHash.addData(source.read(1024*1024));}
    QJsonArray references,pipelines,records;
    for(const auto &ref:referencesInOrder) {
        const auto *page=document.page(ref.number);
        core::OcrDocument snapshot;snapshot.sourcePath=QFileInfo(document.sourcePath).fileName();snapshot.totalPages=document.totalPages;snapshot.pages={*page};
        references.append(QJsonObject{{"page",ref.number},{"split",ref.heldOut?"held_out":"calibration"},
            {"text",core::benchmarkText(*page)},{"sha256",digest(QJsonDocument(snapshot.toJson()).toJson(QJsonDocument::Compact))},
            {"page_snapshot",snapshot.toJson()}});
    }
    for(const auto &p:plan.pipelines)pipelines.append(p.toJson());
    QJsonObject report{{"schema","yagf-experiment/1"},{"application_version",QCoreApplication::applicationVersion()},{"started_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"source_sha256",QString::fromLatin1(sourceHash.result().toHex())},{"reference_origin",document.engine=="synthetic_rendered_ground_truth"?"synthetic_rendered_ground_truth":"verified_project_or_confirmed_snapshot"},{"reference_policy","Explicitly verified text; disjoint pages for calibration and held-out evaluation"},
        {"scoring","NFC; whitespace collapsed; case and punctuation preserved; Unicode code points"},
        {"host",localHost()},{"repetitions",plan.repetitions},{"warmups_per_pipeline",plan.warmups},{"references",references},{"pipelines",pipelines},
        {"timing_scope","End-to-end recognition, including PNG/HTTP; source render and preprocessing measured separately; model startup excluded"},
        {"structural_scope","Word-box overlap and reading-order diagnostics where coordinate frame and granularity agree; table-cell accuracy not measured"}};
    qint64 reportBytes=QJsonDocument(report).toJson(QJsonDocument::Compact).size();
    if(reportBytes>32*1024*1024)return std::unexpected(QStringLiteral("Reference snapshots exceed the 32 MiB experiment budget; select fewer pages."));
    bool budgetExceeded=false;
    QTemporaryDir temporary;if(!temporary.isValid())return std::unexpected(QStringLiteral("Could not create experiment files."));
    std::atomic_bool cancelled{false};std::stop_callback cancellation(stop,[&]{cancelled=true;});
    const int total=plan.pipelines.size()*plan.references.size()*plan.repetitions;int completed=0;
    // Alternate the pipeline order on successive repetitions to reduce a fixed warm-cache bias.
    // std::vector owns engines because Qt containers require copyable values on detach.
    std::vector<std::unique_ptr<processing::IOcrEngine>> providers;
    QHash<QString,QPair<int,bool>> sampleHashes;
    QVector<bool> warmed(plan.pipelines.size(),false);
    QVector<QJsonObject> versions;
    for(const auto &p:plan.pipelines){providers.push_back(factory?factory(p):makeEngine(p));versions.append(engineVersion(p));}
    for(int repetition=0;repetition<plan.repetitions && !stop.stop_requested() && !budgetExceeded;++repetition)
      for(int offset=0;offset<plan.pipelines.size() && !stop.stop_requested() && !budgetExceeded;++offset) {
        const int index=(offset+repetition)%plan.pipelines.size();const auto &p=plan.pipelines[index];auto *engine=providers[index].get();
        for(const auto &ref:referencesInOrder) {
            if(stop.stop_requested() || budgetExceeded)break;
            QJsonObject record{{"pipeline",p.id},{"page",ref.number},{"split",ref.heldOut?"held_out":"calibration"},{"repetition",repetition+1},
                {"provenance",versions[index]}};
            const auto finish=[&](const QString &error={}) {
                if(!error.isEmpty())record["error"]=error;
                reportBytes+=QJsonDocument(record).toJson(QJsonDocument::Compact).size();
                if(reportBytes>128*1024*1024){record.remove("hypothesis");record["error"]="Report exceeded 128 MiB; choose fewer runs";budgetExceeded=true;}
                records.append(record);++completed;
                if(progress)progress(completed,total,QStringLiteral("%1 · page %2").arg(p.label).arg(ref.number));
            };
            if(!engine){finish(QStringLiteral("Unknown provider"));continue;}
            const auto *original=document.page(ref.number);
            core::OcrDocument sourceOnly;sourceOnly.sourcePath=document.sourcePath;sourceOnly.totalPages=document.totalPages;
            sourceOnly.rendering={p.dpi>0?p.dpi:p.dpi==0?original->renderDpi:150,p.dpi==-1};
            QElapsedTimer prep;prep.start();
            auto raw=processing::readExportPage(sourceOnly,ref.number,temporary,cancelled);
            if(!raw){finish(raw.error().error.isEmpty()?QStringLiteral("Cancelled"):raw.error().error);continue;}
            const int dpi=processing::isPagedSource(document.sourcePath)?qRound(raw->dotsPerMeterY()*.0254):150;
            auto settings=original->preprocessing;
            if(!p.savedFilters){settings.autoContrast=false;settings.denoise=false;settings.adaptiveBinarization=false;settings.inkThreshold=0;}
            settings.autoContrast|=p.contrast;settings.denoise|=p.denoise;
            settings.adaptiveBinarization|=p.binarize;
            if(p.autoDeskew) {
                auto binary=processing::ImageProcessor::apply(*raw,{.adaptiveBinarization=true});
                if(!binary){finish(binary.error());continue;}
                auto rotation=processing::detectPageOrientation(*binary,stop);
                if(rotation)settings.quarterTurns=rotation->quarterTurns;
                else record["orientation_note"]=rotation.error().message;
                auto upright=processing::ImageProcessor::apply(*raw,{.quarterTurns=settings.quarterTurns});
                if(!upright){finish(upright.error());continue;}
                auto assessment=processing::ImageProcessor::assess(*upright);
                if(assessment)settings.deskewDegrees=assessment->skewDegrees;
            }
            auto input=processing::ImageProcessor::apply(*raw,settings);
            if(!input){finish(input.error());continue;}
            record["preparation_ms"]=prep.nsecsElapsed()/1000000.0;
            record["dpi"]=dpi;record["width"]=input->width();record["height"]=input->height();record["preprocessing"]=preprocessing(settings);
            QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);if(!input->save(&buffer,"PNG")){finish("Could not hash prepared image");continue;}
            record["input_png_sha256"]=digest(png);
            const QString inputKey=p.id+":"+digest(png);
            if(sampleHashes.contains(inputKey) && sampleHashes[inputKey].first!=ref.number && sampleHashes[inputKey].second!=ref.heldOut) {
                finish(QStringLiteral("Calibration and held-out images are identical; choose independent reference pages."));continue;
            }
            sampleHashes.insert(inputKey,{ref.number,ref.heldOut});
            const processing::OcrOptions options{.language=p.language,.pageNumber=ref.number,.collectBenchmarkMetrics=true};
            if(!warmed[index]) {
                // Warm only on calibration images. Held-out transcriptions never become hints.
                const auto calibrationRef=std::find_if(plan.references.begin(),plan.references.end(),[](const auto &r){return !r.heldOut;});
                if(ref.number==calibrationRef->number) {
                    for(int w=0;w<plan.warmups && !stop.stop_requested();++w) {
                        auto warm=engine->recognize(*input,options,stop);
                        if(!warm){record["warmup_error"]=warm.error().message;break;}
                    }
                    warmed[index]=true;
                }
            }
            QElapsedTimer clock;clock.start();auto outcome=engine->recognize(*input,options,stop);
            record["recognition_ms"]=clock.nsecsElapsed()/1000000.0;
            record["pipeline_ms"]=record["recognition_ms"].toDouble()+record["preparation_ms"].toDouble();
            record["run_details"]=engine->lastRunDetails();
            if(!outcome){finish(outcome.error().cancelled?QStringLiteral("Cancelled"):outcome.error().message);continue;}
            if(outcome->number!=ref.number || outcome->width!=input->width() || outcome->height!=input->height()){finish("Inconsistent provider geometry");continue;}
            auto truth=core::rescalePageGeometry(*original,input->width(),input->height(),raw->width(),raw->height(),dpi);
            const bool sameFrame=settings.quarterTurns==original->preprocessing.quarterTurns &&
                std::abs(settings.deskewDegrees-original->preprocessing.deskewDegrees)<.01;
            auto scores=core::scoreRecognition(truth,*outcome,sameFrame && engine->capabilities().supportsWordBoxes,stop);
            if(!scores){finish(scores.error());continue;}
            record["scores"]=scores->toJson();
            outcome->sourceWidth=raw->width();outcome->sourceHeight=raw->height();outcome->renderDpi=dpi;outcome->preprocessing=settings;
            core::OcrDocument hypothesis;hypothesis.sourcePath=QFileInfo(document.sourcePath).fileName();hypothesis.totalPages=document.totalPages;hypothesis.pages={*outcome};record["hypothesis"]=hypothesis.toJson();
            finish();
        }
      }
    report["records"]=records;report["finished_utc"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    report["cancelled"]=stop.stop_requested();report["completed_runs"]=completed;report["expected_runs"]=total;
    const auto summary=summarizeExperiment(report);report["summary"]=summary;
    QString winner;std::tuple<double,double,double> best{INFINITY,INFINITY,INFINITY};
    for(const auto &value:summary){const auto row=value.toObject();if(!row["complete"].toBool())continue;const auto cal=row["calibration"].toObject();const auto rank=std::tuple{cal["cer"].toDouble(INFINITY),cal["wer"].toDouble(INFINITY),cal["pipeline_mean_ms"].toDouble(INFINITY)};if(rank<best){best=rank;winner=row["pipeline"].toString();}}
    report["advisory_recommendation"]=(stop.stop_requested() || budgetExceeded)?QString():winner;
    if(budgetExceeded)report["incomplete_reason"]="Report size limit reached";
    report["recommendation_basis"]="Lowest pooled calibration CER among fully completed pipelines; ties use calibration WER and total preparation/recognition latency; held-out results are not used for selection. No engine is switched automatically.";
    return report;
}
QJsonArray summarizeExperiment(const QJsonObject &report)
{
    QJsonArray summary;const auto records=report["records"].toArray();
    const int expected=report["references"].toArray().size()*report["repetitions"].toInt();
    for(const auto &value:report["pipelines"].toArray()) {
        const auto p=value.toObject();QJsonObject row{{"pipeline",p["id"]},{"label",p["label"]}};
        int successes=0,failures=0;QVector<double> durations;double elapsed=0,peak=0;
        for(const auto &split:{QStringLiteral("calibration"),QStringLiteral("held_out")}) {
            qint64 ce=0,cn=0,we=0,wn=0;int samples=0;double splitMs=0,splitPreparationMs=0;
            for(const auto &v:records) {
                const auto r=v.toObject();if(r["pipeline"]!=p["id"] || r["split"]!=split)continue;
                if(r.contains("error")){++failures;continue;}
                const auto scores=r["scores"].toObject(),c=scores["cer"].toObject(),w=scores["wer"].toObject();
                ce+=c["errors"].toInteger();cn+=c["reference_units"].toInteger();we+=w["errors"].toInteger();wn+=w["reference_units"].toInteger();++samples;++successes;
                const double ms=r["recognition_ms"].toDouble();durations.append(ms);elapsed+=ms;splitMs+=ms;splitPreparationMs+=r["preparation_ms"].toDouble();
                const auto memory=r["run_details"].toObject()["peak_rss_bytes"];if(memory.isDouble())peak=std::max(peak,memory.toDouble());
            }
            row[split]=QJsonObject{{"runs",samples},{"cer",cn?QJsonValue(double(ce)/cn):QJsonValue::Null},{"wer",wn?QJsonValue(double(we)/wn):QJsonValue::Null},
                {"mean_ms",samples?QJsonValue(splitMs/samples):QJsonValue::Null},{"preparation_mean_ms",samples?QJsonValue(splitPreparationMs/samples):QJsonValue::Null},{"pipeline_mean_ms",samples?QJsonValue((splitMs+splitPreparationMs)/samples):QJsonValue::Null},{"character_errors",ce},{"reference_characters",cn},{"word_errors",we},{"reference_words",wn}};
        }
        std::sort(durations.begin(),durations.end());
        row["complete"]=successes==expected && failures==0;row["failed_runs"]=failures;
        row["mean_ms"]=successes?QJsonValue(elapsed/successes):QJsonValue::Null;
        row["p95_ms"]=durations.isEmpty()?QJsonValue::Null:QJsonValue(durations[std::clamp(int(std::ceil(durations.size()*.95))-1,0,int(durations.size()-1))]);
        row["pages_per_minute"]=elapsed>0?QJsonValue(successes*60000/elapsed):QJsonValue::Null;
        row["peak_rss_bytes"]=peak>0?QJsonValue(peak):QJsonValue::Null;summary.append(row);
    }
    return summary;
}
}
