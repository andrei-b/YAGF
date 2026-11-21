#include "recognitionrecorder.h"
#include "tesseractengine.h"
#include <QScopeGuard>
#include "documentprocessor.h"
#include "parallelocr.h"
#include "ocrreadings.h"
#include "ocrreadingsdialog.h"
#include "projectfile.h"
#include "imageprocessor.h"
#include "pagegeometry.h"
#include "correctionretention.h"
#include "wordeditor.h"
#include "documentcommands.h"
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUndoStack>
#include <QTemporaryDir>
#include <QtTest>
#include <stdexcept>

using namespace yagf;
namespace {
class Engine final:public processing::IOcrEngine {
public:
    QString name="fixture";
    enum Mode { Success, Failed, Invalid, Throws, Cancelled } mode=Success;
    QString id() const override {return name;}
    QString modelIdentifier() const override {return "document-model";}
    processing::EngineCapabilities capabilities() const override {return {.supportsWordBoxes=true};}
    QJsonObject lastStructuredOutput() const override {return {{"error","original failure"},{"native",QJsonArray{1,2,3}}};}
    std::expected<core::RecognitionResult,processing::OcrError> recognizeResult(const QImage &image,const processing::OcrOptions &options,std::stop_token) override {
        if(mode==Failed)return std::unexpected(processing::OcrError{"engine failed"});
        if(mode==Throws)throw std::runtime_error("engine exception");
        if(mode==Cancelled)return std::unexpected(processing::OcrError{"cancelled",true});
        core::RecognitionResult r;r.engineId=id();r.modelVersion="revision-7";
        r.provenance={.model="document-model",.task="ocr",.language=options.language,.pageNumber=options.pageNumber,.imageSize=image.size()};
        core::TextRegion text;text.text="Other";text.bounds=mode==Invalid?QRect(9000,9000,20,10):QRect(5,5,20,10);text.granularity=core::TextGranularity::Word;
        text.block=text.paragraph=text.line=1;r.text.append(text);
        r.structuredOutput={{"model_hash",QString(64,'a')},{"hierarchy",QJsonArray{QJsonObject{{"native_field","preserved"}}}},
            {"warnings",QJsonArray{"low confidence"}},{"run_details",QJsonObject{{"inference_ms",17.5}}}};
        return r;
    }
};
QImage image() {QImage input(200,120,QImage::Format_RGB32);input.fill(Qt::white);input.setPixelColor(20,20,Qt::black);return input;}
processing::OcrOptions context(QSize size={200,120}) {
    processing::OcrOptions options;options.language="eng";options.historyCoordinates.sourceSize=size;
    options.history=std::make_shared<processing::RecognitionRecorder>(QString(64,'b'),core::preprocessingEvidence({},150));return options;
}
core::OcrPage pageWithHistory() {
    Engine engine;auto options=context();auto result=engine.recognize(image(),options,{});
    if(!result)qFatal("fixture failed");result->coordinates=options.historyCoordinates;return *result;
}
core::OcrReading companion(const core::OcrPage &page) {
    core::OcrReading reading;reading.source="fixture";reading.label="Other engine";reading.engine="fixture";reading.language="eng";
    reading.area={0,0,page.width,page.height};reading.items={{{5,5,20,10},"Other",1,1,1,false}};
    for(const auto &run:page.recognitionHistory.runs)reading.historyIds.append(run.id);return reading;
}
}
class RecognitionHistoryTests:public QObject {
    Q_OBJECT
private slots:
    void tesseractVersionModelHashAndOriginalTsvAreCaptured() {
        QTemporaryDir temporary;QFile weights(temporary.filePath("eng.traineddata"));
        QVERIFY(weights.open(QIODevice::WriteOnly));const QByteArray model("fixture weights");QCOMPARE(weights.write(model),model.size());weights.close();
        QFile executable(temporary.filePath("tesseract"));QVERIFY(executable.open(QIODevice::WriteOnly));
        const QByteArray script=R"PY(#!/usr/bin/env python3
import pathlib, sys
if '--version' in sys.argv:
    print('tesseract fixture-1')
elif '--list-langs' in sys.argv:
    print('List of available languages in "' + str(pathlib.Path(__file__).parent) + '" (1):')
    print('eng')
else:
    print('level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext')
    print('5\t1\t1\t1\t1\t1\t5\t5\t20\t10\t95\tfixture')
)PY";
        QCOMPARE(executable.write(script),script.size());executable.close();QVERIFY(executable.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        const auto oldPath=qgetenv("PATH");const auto restore=qScopeGuard([&]{qputenv("PATH",oldPath);});qputenv("PATH",temporary.path().toUtf8()+':'+oldPath);
        processing::TesseractEngine engine;auto options=context();auto result=engine.recognize(image(),options,{});QVERIFY2(result,qPrintable(result?QString():result.error().message));
        const auto run=options.history->snapshot().runs[0];QCOMPARE(run.version,"tesseract fixture-1");QCOMPARE(run.modelHash,core::sha256(model));
        QVERIFY(run.output["structured_output"].toObject()["tsv"].toString().contains("95\tfixture"));
    }
    void exactCropAndBothCoordinateFrames() {
        Engine engine;auto options=context();
        auto mapping=processing::ImageProcessor::coordinatePipeline({120,200},{.quarterTurns=1});QVERIFY(mapping);
        options.historyCoordinates=*mapping;
        auto cropped=processing::croppedOcrOptions(options,{30,20,70,40},8);
        QImage pixels(86,56,QImage::Format_RGB32);pixels.fill(Qt::white);
        auto result=engine.recognize(pixels,cropped,{});QVERIFY(result);
        const auto history=options.history->snapshot();QCOMPARE(history.runs.size(),1);const auto &run=history.runs[0];
        QCOMPARE(run.engine,"fixture");QCOMPARE(run.model,"document-model");QCOMPARE(run.version,"revision-7");QCOMPARE(run.modelHash,QString(64,'a'));
        QCOMPARE(run.sourceHash,QString(64,'b'));QCOMPARE(run.inputHash,core::sha256(run.inputPng));
        QCOMPARE(QImage::fromData(run.inputPng),pixels);QCOMPARE(run.processedCrop,QRect(22,12,86,56));
        QCOMPARE(run.sourceToInput.outputSize(),QSizeF(86,56));QVERIFY(!run.sourceCrop.isEmpty());
        auto source=run.sourceToInput.toSource({8,8});auto expected=mapping->toSource({30,20});QVERIFY(source && expected);QCOMPARE(*source,*expected);
        QCOMPARE(run.runtime["inference_ms"].toDouble(),17.5);QVERIFY(run.warnings.contains("low confidence"));
        QVERIFY(run.output["structured_output"].toObject().contains("hierarchy"));QVERIFY(!run.startedUtc.isEmpty());
        QCOMPARE(run.status,"succeeded");QVERIFY(core::validateRecognitionHistory(history));
        auto altered=context();altered.history=options.history;altered.singleTextLine=true;
        QVERIFY(engine.recognize(image(),altered,{}));
        QVERIFY(options.history->snapshot().runs.last().preprocessingHash!=run.preprocessingHash);
    }
    void projectRoundTripDeduplicatesInputsAndFrames() {
        auto page=pageWithHistory();auto second=page.recognitionHistory.runs[0];second.id="second-run";page.recognitionHistory.runs.append(second);
        page.ocrReadings.append(companion(page));page.words[0].text="Primary";
        auto accepted=core::acceptOcrReading(page,{0},page.ocrReadings[0],"Other");QVERIFY(accepted);
        core::recordReadingDecision(*accepted,*accepted,{0},accepted->ocrReadings[0],"Other","rejected");
        core::OcrDocument book;book.sourcePath="fixture.png";book.pages.append(*accepted);
        auto json=book.toJson();const auto saved=json["pages"].toArray()[0].toObject();
        QCOMPARE(saved["recognition_history"].toObject()["inputs"].toObject().size(),1);
        QCOMPARE(saved["recognition_frames"].toObject().size(),1);
        core::OcrDocument loaded;QString error;QVERIFY2(core::OcrDocument::fromJson(json,&loaded,&error),qPrintable(error));
        QCOMPARE(core::recognitionHistoryJson(loaded.pages[0].recognitionHistory),core::recognitionHistoryJson(accepted->recognitionHistory));
        QCOMPARE(loaded.pages[0].ocrReadings[0].historyIds,accepted->ocrReadings[0].historyIds);
        QTemporaryDir temporary;const auto path=temporary.filePath("history.yagf");QVERIFY(core::ProjectFile::save(path,book,&error));
        QVERIFY2(core::ProjectFile::load(path,&loaded,&error),qPrintable(error));QCOMPARE(loaded.pages[0].words[0].text,"Other");
    }
    void failedCancelledAndInvalidNativeOutputAreRetained() {
        Engine engine;auto options=context();
        for(const auto mode:{Engine::Failed,Engine::Invalid,Engine::Throws,Engine::Cancelled}) {
            engine.mode=mode;QVERIFY(!engine.recognize(image(),options,{}));
        }
        auto history=options.history->snapshot();QCOMPARE(history.runs.size(),4);
        QCOMPARE(history.runs[0].output["error"].toString(),"original failure");
        QCOMPARE(history.runs[1].output["text"].toArray()[0].toObject()["box"].toArray()[0].toInt(),9000);
        QVERIFY(history.runs[2].error.contains("exception"));QCOMPARE(history.runs[3].status,"cancelled");
        auto saved=core::readRecognitionHistory(core::recognitionHistoryJson(history));QVERIFY(saved);QCOMPARE(saved->runs.size(),4);
        engine.mode=Engine::Failed;QVERIFY(!engine.recognize({},options,{}));QVERIFY(core::validateRecognitionHistory(options.history->snapshot()));
        QVERIFY(core::readRecognitionHistory(core::recognitionHistoryJson(options.history->snapshot())));
    }
    void corruptedEvidenceIsRejectedAndSaveKeepsOriginalFile() {
        auto page=pageWithHistory();auto json=core::recognitionHistoryJson(page.recognitionHistory);
        auto runs=json["runs"].toArray();auto run=runs[0].toObject();run["input_sha256"]=QString(64,'c');runs[0]=run;json["runs"]=runs;
        QVERIFY(!core::readRecognitionHistory(json));
        auto wrong=page.recognitionHistory;wrong.runs[0].preprocessing["render_dpi"]=600;QVERIFY(!core::validateRecognitionHistory(wrong));
        wrong=page.recognitionHistory;auto duplicate=wrong.runs[0];duplicate.id="different";duplicate.inputPng.append("tampered");wrong.runs.append(duplicate);
        QVERIFY(!core::validateRecognitionHistory(wrong));
        core::OcrDocument book;book.sourcePath="fixture.png";book.pages.append(page);QTemporaryDir temporary;const auto path=temporary.filePath("book.yagf");QString error;
        QVERIFY(core::ProjectFile::save(path,book,&error));QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));auto original=file.readAll();file.close();
        book.pages[0].recognitionHistory=wrong;QVERIFY(!core::ProjectFile::save(path,book,&error));QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),original);
    }
    void rerunsAndGeometryChangesPreserveHistoricalEvidence() {
        auto original=pageWithHistory();const auto snapshot=core::recognitionHistoryJson(original.recognitionHistory);
        auto mapping=processing::ImageProcessor::coordinatePipeline({200,120},{.quarterTurns=1,.bendPercent=2});QVERIFY(mapping);
        auto rotated=core::remapPageGeometry(original,*mapping);QVERIFY(rotated);
        QCOMPARE(core::recognitionHistoryJson(rotated->recognitionHistory),snapshot);
        auto fresh=pageWithHistory();core::retainCorrections(fresh,*rotated,false);
        QCOMPARE(fresh.recognitionHistory.runs.size(),2);
        const QJsonValue old=core::recognitionHistoryJson(fresh.recognitionHistory)["runs"].toArray()[0];QCOMPARE(old,snapshot["runs"].toArray()[0]);
    }
    void reviewDecisionsAreExplicitUndoableAndSaved() {
        auto page=pageWithHistory();page.words[0].text="Primary";page.ocrReadings.append(companion(page));
        ui::OcrReadingsDialog dialog(page,image());auto *reject=dialog.findChild<QPushButton *>("rejectOcrReading");QVERIFY(reject && reject->isEnabled());
        reject->click();QCOMPARE(dialog.editedPage().words[0].text,"Primary");QCOMPARE(dialog.editedPage().recognitionHistory.decisions.size(),1);
        QCOMPARE(dialog.editedPage().recognitionHistory.decisions[0].decision,"rejected");QVERIFY(page.recognitionHistory.decisions.isEmpty());
        dialog.findChild<QPushButton *>("useOcrReading")->click();QCOMPARE(dialog.editedPage().words[0].text,"Other");
        QCOMPARE(dialog.editedPage().recognitionHistory.decisions.size(),2);QCOMPARE(dialog.editedPage().recognitionHistory.decisions.last().decision,"accepted");
        core::OcrDocument book;book.sourcePath="fixture.png";book.pages.append(page);QUndoStack undo;undo.push(new ui::ReplacePageCommand(book,1,dialog.editedPage(),"Review"));
        QCOMPARE(book.pages[0].recognitionHistory.decisions.size(),2);undo.undo();QVERIFY(book.pages[0].recognitionHistory.decisions.isEmpty());undo.redo();
        QCOMPARE(book.pages[0].recognitionHistory.decisions.size(),2);
        QTimer::singleShot(0,&dialog,[&]{auto *history=dialog.findChild<QDialog *>("recognitionHistoryDialog");QVERIFY(history);auto *output=history->findChild<QPlainTextEdit *>("recognitionRawOutput");QVERIFY(output);QVERIFY(output->toPlainText().contains("native_field"));history->reject();});
        dialog.findChild<QPushButton *>("recognitionHistory")->click();
    }
    void specialistAcceptanceAndRejectionKeepRunLinks() {
        auto page=pageWithHistory();auto &word=page.words[0];word.text=word.originalText="Primary";word.alternative={"fixture","Other",{"Primary"},false,{page.recognitionHistory.runs[0].id}};
        auto accepted=core::WordEditor::acceptSpecialist(page,0,"Other");QVERIFY(accepted);QCOMPARE(accepted->recognitionHistory.decisions.last().decision,"accepted");
        auto rejected=core::WordEditor::acceptSpecialist(page,0,"Primary");QVERIFY(rejected);QCOMPARE(rejected->recognitionHistory.decisions.last().decision,"rejected");
        core::OcrDocument book;book.sourcePath="fixture.png";book.pages.append(page);core::OcrDocument restored;QString error;QVERIFY2(core::OcrDocument::fromJson(book.toJson(),&restored,&error),qPrintable(error));
        QCOMPARE(restored.pages[0].words[0].alternative.historyIds,word.alternative.historyIds);
    }
    void cropReviewLinksOnlyTheRunThatProducedTheMatchedItem() {
        auto page=pageWithHistory();auto old=page.recognitionHistory.runs[0];old.id="old-run";old.session="older";page.recognitionHistory.runs.prepend(old);
        auto reading=companion(page);reading.items[0].historyId=page.recognitionHistory.runs.last().id;
        core::recordReadingDecision(page,page,{0},reading,"Other","rejected");
        QCOMPARE(page.recognitionHistory.decisions.size(),1);QCOMPARE(page.recognitionHistory.decisions[0].runId,reading.items[0].historyId);
    }
    void failedJobPublishesEvidenceWithoutPublishingNewText() {
        QTemporaryDir temporary;const auto path=temporary.filePath("page.png");QVERIFY(image().save(path));
        core::OcrDocument book;book.sourcePath=path;book.pages.append(pageWithHistory());const auto before=book.toJson();
        processing::DocumentProcessor processor(nullptr,&book);auto engine=std::make_unique<Engine>();engine->mode=Engine::Failed;processor.setEngine(std::move(engine));
        QSignalSpy evidence(&processor,&processing::DocumentProcessor::recognitionHistoryReady),failed(&processor,&processing::DocumentProcessor::failed),done(&processor,&processing::DocumentProcessor::recognized);
        processor.recognize(path,"eng",1);QTRY_COMPARE_WITH_TIMEOUT(failed.size(),1,3000);QCOMPARE(done.size(),0);QCOMPARE(evidence.size(),1);
        auto page=evidence.first()[0].value<core::OcrPage>();QCOMPARE(page.recognitionHistory.runs.size(),2);QCOMPARE(page.recognitionHistory.runs[0].status,"failed");
        QVERIFY(core::validateRecognitionHistory(page.recognitionHistory));QCOMPARE(book.toJson(),before);
    }
    void companionCropsKeepIndependentRunIds() {
        auto options=context();std::vector<processing::ComparisonEngine> engines;
        processing::OcrService service;service.provider="fixture";service.label="Other";service.model="model";
        engines.push_back({service,std::make_unique<Engine>()});
        processing::ParallelOcrRun parallel(engines,image(),options,{{10,10,70,30},{100,70,60,30}},{});
        auto readings=parallel.finish();QCOMPARE(readings.size(),1);QCOMPARE(readings[0].historyIds.size(),2);
        const auto history=options.history->snapshot();QCOMPARE(history.runs.size(),2);QCOMPARE(history.runs[0].role,"companion");
        QCOMPARE(history.runs[0].processedCrop,QRect(10,10,70,30));QCOMPARE(history.runs[1].processedCrop,QRect(100,70,60,30));
        QCOMPARE(history.runs[0].sourceToInput.toSource({5,5}).value(),QPointF(15,15));
    }
};
QTEST_MAIN(RecognitionHistoryTests)
#include "recognitionhistory_tests.moc"
