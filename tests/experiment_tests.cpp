#include "experimentrunner.h"
#include "projectfile.h"
#include <QGuiApplication>
#include <QPdfWriter>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>
using namespace yagf;
static core::OcrPage reference(int number,QString text)
{
    core::OcrPage page;page.number=number;page.id=QStringLiteral("page-%1").arg(number);page.engineId="fixture";
    page.width=page.sourceWidth=500;page.height=page.sourceHeight=500;page.language="eng";
    core::OcrWord word;word.page=number;word.text=word.originalText=text;word.bounds={20,30,120,20};word.reviewed=true;word.confidence=100;
    page.words={word};page.rebuildHierarchy();return page;
}
struct Calls {QString id;int page;};
class FixtureEngine final:public processing::IOcrEngine {
public:
    FixtureEngine(QString id,std::shared_ptr<QVector<Calls>> calls,std::function<void()> action={}):name(id),calls(calls),action(action){}
    QString id()const override{return "fixture";}
    processing::EngineCapabilities capabilities()const override{return {.supportsWordBoxes=true};}
    QJsonObject lastRunDetails()const override{return {{"peak_rss_bytes",123456},{"peak_rss_scope","fixture"}};}
    std::expected<core::OcrPage,processing::OcrError> recognize(const QImage &image,const processing::OcrOptions &options,std::stop_token)override {
        Q_ASSERT(options.userWords.isEmpty() && !options.preserveFormatting && options.collectBenchmarkMetrics);
        calls->append({name,options.pageNumber});if(action)action();
        if(name=="fail")return std::unexpected(processing::OcrError{"Fixture failure"});
        auto page=reference(options.pageNumber,options.pageNumber==1?"alpha beta":"gamma delta");page.width=page.sourceWidth=image.width();page.height=page.sourceHeight=image.height();for(auto &word:page.words){word.reviewed=false;if(name=="bad")word.text="wrong reading";}return page;
    }
private:QString name;std::shared_ptr<QVector<Calls>> calls;std::function<void()> action;
};
class ExperimentTests:public QObject {
    Q_OBJECT
private slots:
    void unicodeScoringAndLimits(){
        auto ref=reference(1,QString::fromUtf8("A café 😀"));auto hyp=ref;hyp.words[0].text=QString::fromUtf8("A café 😀");
        auto score=core::scoreRecognition(ref,hyp,false);QVERIFY(score);QCOMPARE(score->characters.errors,0);QCOMPARE(score->characters.referenceUnits,8);
        hyp.words[0].text=QString::fromUtf8("a cafe 😀 extra");score=core::scoreRecognition(ref,hyp,false);QVERIFY(score);QCOMPARE(score->characters.errors,8);QCOMPARE(score->words.errors,3);
        hyp.words.clear();score=core::scoreRecognition(ref,hyp,false);QVERIFY(score);QCOMPARE(score->characters.errors,8);QCOMPARE(score->words.errors,3);
        ref.words[0].text=QString(12000,'a');hyp.words={ref.words[0]};hyp.words[0].text=QString(12000,'b');QVERIFY(!core::scoreRecognition(ref,hyp,false));
    }
    void weightedScoringSplitAndPreservation(){
        QTemporaryDir dir;QVERIFY(dir.isValid());const QString path=dir.filePath("source.pdf");
        {QPdfWriter pdf(path);pdf.setResolution(150);pdf.setPageSize(QPageSize(QSizeF(240,240),QPageSize::Point));pdf.setPageMargins(QMarginsF(0,0,0,0));QPainter p(&pdf);p.fillRect(0,0,500,500,Qt::white);p.drawText(20,40,"alpha beta");QVERIFY(pdf.newPage());p.fillRect(0,0,500,500,Qt::white);p.drawText(20,40,"gamma delta");}
        core::OcrDocument doc;doc.sourcePath=path;doc.totalPages=2;doc.replacePage(reference(1,"alpha beta"));doc.replacePage(reference(2,"gamma delta"));const auto snapshot=doc.toJson();
        experiments::ExperimentPlan plan;plan.references={{2,true},{1,false}};plan.pipelines={{.id="bad",.label="Bad"},{.id="good",.label="Good"}};
        plan.pipelines[0].token="never-save-this";auto calls=std::make_shared<QVector<Calls>>();
        auto factory=[calls](const auto &p){return std::make_unique<FixtureEngine>(p.id,calls);};
        auto report=experiments::runExperiment(doc,plan,{}, {},factory);QVERIFY2(report,qPrintable(report?QString():report.error()));
        QCOMPARE(doc.toJson(),snapshot);QCOMPARE(report->value("advisory_recommendation").toString(),"good");QCOMPARE(calls->size(),10);QCOMPARE(calls->first().page,1);
        QVERIFY(!QJsonDocument(*report).toJson().contains("never-save-this"));QCOMPARE(report->value("records").toArray().size(),8);
        const auto rows=report->value("summary").toArray();QVERIFY(rows[1].toObject()["complete"].toBool());QCOMPARE(rows[1].toObject()["held_out"].toObject()["cer"].toDouble(),0.0);
        QCOMPARE(rows[1].toObject()["peak_rss_bytes"].toDouble(),123456.0);
        plan.references={{1,false},{1,true}};QVERIFY(!experiments::runExperiment(doc,plan,{}, {},factory));
        plan.references={{1,false},{2,true}};doc.pages[0].words[0].reviewed=false;QVERIFY(!experiments::runExperiment(doc,plan,{}, {},factory));doc.pages[0].words[0].reviewed=true;
        plan.pipelines[0].id="fail";report=experiments::runExperiment(doc,plan,{}, {},factory);QVERIFY(report);QVERIFY(!report->value("summary").toArray()[0].toObject()["complete"].toBool());QCOMPARE(report->value("advisory_recommendation").toString(),"good");
        std::stop_source stop;calls->clear();plan.warmups=0;
        auto cancelFactory=[&](const auto &p){return std::make_unique<FixtureEngine>(p.id,calls,[&]{stop.request_stop();});};
        report=experiments::runExperiment(doc,plan,stop.get_token(),{},cancelFactory);QVERIFY(report);QVERIFY(report->value("cancelled").toBool());QVERIFY(report->value("advisory_recommendation").toString().isEmpty());QCOMPARE(calls->size(),1);
    }
    void geometryDiagnostics(){
        auto ref=reference(1,"alpha");auto other=ref.words[0];other.id="second";other.text="beta";other.bounds.translate(150,0);ref.words.append(other);ref.rebuildHierarchy();
        auto hyp=ref;std::swap(hyp.words[0],hyp.words[1]);auto score=core::scoreRecognition(ref,hyp,true);QVERIFY(score);QCOMPARE(score->geometry["matched_boxes"].toInt(),2);QCOMPARE(score->geometry["reading_order_concordance"].toDouble(),0.0);
        hyp.words[0].layoutBlock=true;score=core::scoreRecognition(ref,hyp,true);QVERIFY(score && !score->geometry["available"].toBool());
    }
};
QTEST_MAIN(ExperimentTests)
#include "experiment_tests.moc"
