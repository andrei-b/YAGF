#include "documentprocessor.h"
#include "mathocrengine.h"
#include "smartblocks.h"
#include "tesseractengine.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <condition_variable>
#include <thread>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#endif

using namespace yagf;
class RecognitionLivenessTests:public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QByteArray oldPath,oldData;
    core::OcrPage raw;
    QImage image;
    QString scan;
    bool executable(const QString &path,const QByteArray &body) {
        QFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(body)!=body.size()) return false;
        f.close(); return f.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
    }
    void backend(const QByteArray &mode) {
        QFile::remove(temporary.filePath("helper.pid")); QFile::remove(temporary.filePath("calls"));
        const auto prefix=temporary.path().toUtf8();
        const QByteArray body="#!/usr/bin/python3\nimport json,sys,time,os\n"
            "open('"+prefix+"/helper.pid','w').write(str(os.getpid()))\n"
            "open('"+prefix+"/calls','a').write('run\\n')\n"
            "boxes=json.load(open(sys.argv[3])); result=[]\n"
            "for i,(l,t,r,b) in enumerate(boxes):\n"
            " print('YAGF_PROGRESS '+json.dumps({'done':i+1,'total':len(boxes)}),file=sys.stderr,flush=True)\n"
            " if '"+mode+"'=='hang': time.sleep(60)\n"
            " entry={'box':[l,t,r-l,b-t]}\n"
            " if '"+mode+"'=='budget': entry['error']='Formula time budget exhausted'\n"
            " elif '"+mode+"'=='partial' and i==1: entry['error']='Formula crop exceeded its time limit'\n"
            " else: entry['text']='x=1'\n"
            " result.append(entry)\nprint(json.dumps(result))\n";
        QVERIFY(executable(temporary.filePath("yagf2/math-ocr/venv/bin/python"),body));
    }
    void helperIsGone() {
#ifdef Q_OS_UNIX
        QFile pid(temporary.filePath("helper.pid")); QVERIFY(pid.open(QIODevice::ReadOnly));
        bool ok=false; const int value=pid.readAll().toInt(&ok); QVERIFY(ok && value>1);
        errno=0; QCOMPARE(::kill(value,0),-1); QCOMPARE(errno,ESRCH);
#endif
    }
private slots:
    void initTestCase() {
        QVERIFY(temporary.isValid()); oldPath=qgetenv("PATH"); oldData=qgetenv("XDG_DATA_HOME");
        qputenv("PATH",temporary.path().toUtf8()+':'+oldPath); qputenv("XDG_DATA_HOME",temporary.path().toUtf8());
        QVERIFY(QDir().mkpath(temporary.filePath("yagf2/math-ocr/venv/bin")));
        const QByteArray tsv="level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n"
            "5\t1\t1\t1\t1\t1\t10\t10\t80\t12\t95\tBefore\n"
            "5\t1\t1\t1\t2\t1\t10\t40\t50\t12\t95\tx=0\n"
            "5\t1\t1\t1\t3\t1\t10\t70\t50\t12\t95\ty=2\n"
            "5\t1\t1\t1\t4\t1\t10\t100\t80\t12\t95\tAfter\n";
        QVERIFY(executable(temporary.filePath("tesseract"),"#!/bin/sh\ncat <<'YAGF_TSV'\n"+tsv+"YAGF_TSV\n"));
        auto parsed=processing::TesseractEngine::parseTsv(tsv,{200,140},{.language="eng"}); QVERIFY(parsed); raw=*parsed;
        image=QImage(200,140,QImage::Format_RGB32); image.fill(Qt::white);
        QPainter p(&image); for (const auto &w:raw.words) p.fillRect(w.bounds,Qt::black); p.end();
        scan=temporary.filePath("scan.png"); QVERIFY(image.save(scan));
    }
    void cleanupTestCase() {qputenv("PATH",oldPath); qputenv("XDG_DATA_HOME",oldData);}
    void exhaustedFormulaBudgetKeepsNativeWordsAndIds() {
        backend("budget"); QStringList warnings;
        auto result=processing::recognizeSmartBlocks(image,{.language="eng"},{},false,{},true,&raw,&warnings);
        QVERIFY2(result,qPrintable(result ? QString() : result.error().message)); QCOMPARE(result->words.size(),raw.words.size());
        for (int i=0;i<raw.words.size();++i) {
            QCOMPARE(result->words[i].text,raw.words[i].text); QCOMPARE(result->words[i].id,raw.words[i].id);
            QCOMPARE(result->words[i].sourceBoxes,raw.words[i].sourceBoxes); QVERIFY(!result->words[i].layoutBlock);
        }
        QVERIFY(warnings.join(' ').contains("time budget"));
    }
    void slowFormulaDoesNotDiscardOtherCompletedReadings() {
        backend("partial"); QStringList warnings;
        auto result=processing::recognizeSmartBlocks(image,{.language="eng"},{},false,{},true,&raw,&warnings);
        QVERIFY(result); QCOMPARE(result->words[1].text,"x=1"); QVERIFY(result->words[1].layoutBlock);
        QCOMPARE(result->words[2].text,"y=2"); QCOMPARE(result->words[2].id,raw.words[2].id);
        QVERIFY(warnings.join(' ').contains("time limit"));
    }
    void cancellingStalledMathHelperKillsTheChild() {
        backend("hang"); processing::MathOcrEngine engine; std::stop_source stop; QElapsedTimer elapsed; elapsed.start();
        const auto progress=[&](const QString &stage,int complete,int) {if (stage=="Recognizing formulas" && complete) stop.request_stop();};
        auto result=engine.recognizeBoxes(image,{.language="auto",.progress=progress},stop.get_token(),{raw.words[1].bounds});
        QVERIFY(!result); QVERIFY(result.error().cancelled); QVERIFY(elapsed.elapsed()<5000); helperIsGone();
    }
    void hardDeadlineStopsAHelperThatIgnoresItsOwnBudget() {
        backend("hang"); processing::MathOcrEngine engine; std::stop_source stop;
        // A failed assertion must not leave CI waiting for the production five-minute default.
        std::jthread safety([&](std::stop_token end) {
            std::mutex mutex; std::unique_lock lock(mutex); std::condition_variable_any condition;
            condition.wait_for(lock,end,std::chrono::seconds(15),[]{return false;});
            if (!end.stop_requested()) stop.request_stop();
        });
        QElapsedTimer elapsed; elapsed.start();
        auto result=engine.recognizeBoxes(image,{.language="auto"},stop.get_token(),{raw.words[1].bounds},nullptr,20);
        QVERIFY(!result); QVERIFY(!result.error().cancelled); QVERIFY(result.error().message.contains("timed out"));
        QVERIFY(elapsed.elapsed()<14000); helperIsGone();
    }
    void pipelineFinishesWithNativeTextAndReportsProgress() {
        backend("budget"); core::OcrDocument book; book.sourcePath=scan;
        processing::DocumentProcessor processor(nullptr,&book);
        QSignalSpy done(&processor,&processing::DocumentProcessor::recognized),failed(&processor,&processing::DocumentProcessor::failed);
        QSignalSpy stages(&processor,&processing::DocumentProcessor::recognitionStage),warnings(&processor,&processing::DocumentProcessor::specialistWarning);
        processor.recognize(scan,"eng",1); QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failed.isEmpty(),5000);
        QVERIFY(failed.isEmpty()); QVERIFY(!processor.isBusy()); QVERIFY(!warnings.isEmpty());
        const auto result=done.first().first().value<core::OcrPage>(); QCOMPARE(result.words.size(),raw.words.size());
        for (int i=0;i<raw.words.size();++i) QCOMPARE(result.words[i].text,raw.words[i].text);
        bool text=false,math=false;
        for (const auto &stage:stages) {text|=stage[1].toString()=="Reading text with Tesseract"; math|=stage[1].toString()=="Recognizing formulas" && stage[2].toInt()>0;}
        QVERIFY(text); QVERIFY(math);
        QFile calls(temporary.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly)); const auto before=calls.readAll(); calls.close();
        book.useRawOcrOutput=true; done.clear(); processor.recognize(scan,"eng",1);
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty(),5000); QVERIFY(calls.open(QIODevice::ReadOnly)); QCOMPARE(calls.readAll(),before);
    }
    void cancelWorksWhileThePipelineWaitsForFormulaRecognition() {
        backend("hang"); core::OcrDocument book; book.sourcePath=scan;
        processing::DocumentProcessor processor(nullptr,&book);
        QSignalSpy cancelled(&processor,&processing::DocumentProcessor::cancelled),failed(&processor,&processing::DocumentProcessor::failed),done(&processor,&processing::DocumentProcessor::recognized);
        connect(&processor,&processing::DocumentProcessor::recognitionStage,&processor,[&](int,const QString &stage,int complete,int) {
            if (stage=="Recognizing formulas" && complete) processor.cancel();
        });
        processor.recognize(scan,"eng",1); QTRY_VERIFY_WITH_TIMEOUT(!cancelled.isEmpty(),5000);
        QVERIFY(!processor.isBusy()); QVERIFY(done.isEmpty()); QVERIFY(failed.isEmpty()); helperIsGone();
    }
};
QTEST_GUILESS_MAIN(RecognitionLivenessTests)
#include "recognitionliveness_tests.moc"
