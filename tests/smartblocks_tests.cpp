#include "smartblocks.h"
#include "recognitionrecorder.h"
#include "formulatext.h"
#include "wordeditor.h"

#include <QFile>
#include <QDir>
#include <QPainter>
#include <QFontDatabase>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace yagf;

namespace {
struct ConcurrentState {
    std::atomic_int started = 0, active = 0, peak = 0, formulaCalls = 0;
    bool overlap = true, failText = false, failFormula = false, stallFormula = false, stallText = false;
};
class ConcurrentEngine : public processing::IOcrEngine {
    ConcurrentState &state;
    core::OcrPage raw;
    bool formula;
public:
    ConcurrentEngine(ConcurrentState &state, core::OcrPage raw, bool formula)
        : state(state), raw(std::move(raw)), formula(formula) {}
    QString id() const override { return formula ? "test-formulas" : "test-text"; }
    processing::EngineCapabilities capabilities() const override { return {}; }
    std::expected<yagf::core::RecognitionResult, processing::OcrError> recognizeResult(
        const QImage &image, const processing::OcrOptions &, std::stop_token stop) override {
        const int active = ++state.active;
        int peak = state.peak.load();
        while (peak < active && !state.peak.compare_exchange_weak(peak, active)) {}
        struct Finish { std::atomic_int &active; ~Finish() { --active; } } finish{state.active};
        if (formula) ++state.formulaCalls;
        ++state.started;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (state.overlap && state.started < 2 && !stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (state.overlap && state.started < 2 && !stop.stop_requested())
            return std::unexpected(processing::OcrError{"Engines did not overlap"});
        while ((formula ? state.stallFormula : state.stallText) && !stop.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (stop.stop_requested()) return std::unexpected(processing::OcrError{{}, true});
        if ((formula && state.failFormula) || (!formula && state.failText))
            return std::unexpected(processing::OcrError{"Test recognition failure"});
        if (!formula) return yagf::processing::recognitionFromPage(raw);
        core::OcrPage result;
        result.width = image.width(); result.height = image.height();
        result.words.append({1, 1, 1, 1, image.rect(), "x=1", "x=1", 95});
        result.rebuildHierarchy();
        return yagf::processing::recognitionFromPage(result);
    }
};
}

class SmartBlocksTests : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QByteArray originalPath;
    QByteArray originalDataHome;

    void mathBackend(bool failSecond = false)
    {
        const QString directory = temporary.filePath("yagf2/math-ocr/venv/bin");
        QVERIFY(QDir().mkpath(directory));
        QFile script(directory + "/python");
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/usr/bin/python3\nimport json,sys\n"
            "boxes=json.load(open(sys.argv[3]))\nresult=[]\n"
            "for i,(l,t,r,b) in enumerate(boxes):\n"
            " entry={'box':[l,t,r-l,b-t]}\n");
        script.write(failSecond ? " entry.update({'error':'bad crop'} if i==1 else {'text':'x=1'})\n"
                                : " entry['text']='x=1'\n");
        script.write(" result.append(entry)\nprint(json.dumps(result))\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    }

    static core::OcrPage reading(const QVector<core::OcrWord> &words, QSize size)
    {
        core::OcrPage page;
        page.id = "page-1";
        page.width = page.sourceWidth = size.width();
        page.height = page.sourceHeight = size.height();
        page.engineId = "tesseract";
        page.language = "rus";
        page.words = words;
        page.rebuildHierarchy();
        return page;
    }

    static core::OcrWord word(int block, int line, QRect box, const QString &text)
    {
        return {1, block, 1, line, box, text, text, 95};
    }

    static QImage scan(const core::OcrPage &page, QRect missing = {})
    {
        QImage image(page.width, page.height, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        for (const auto &word : page.words) painter.fillRect(word.bounds, Qt::black);
        if (!missing.isEmpty()) painter.fillRect(missing, Qt::black);
        return image;
    }

    static QStringList text(const core::OcrPage &page)
    {
        QStringList result;
        QSet<QString> ids;
        for (const auto &word : page.words) {
            result.append(word.text);
            if (word.id.isEmpty() || ids.contains(word.id)) return {"Invalid word IDs"};
            ids.insert(word.id);
        }
        return result;
    }

private slots:
    void printedMathGlyphs_data()
    {
        QTest::addColumn<QString>("glyph");
        QTest::addColumn<QString>("family");
        QTest::addColumn<bool>("italic");
        QTest::addColumn<bool>("expectFormula");
        for (const auto &s : {"x","z","u","v"}) {
            QTest::newRow(qPrintable("italic "+QString(s))) << QString(s) << QString("Liberation Serif") << true << true;
            QTest::newRow(qPrintable("upright "+QString(s))) << QString(s) << QString("Liberation Serif") << false << false;
        }
        for (const auto &s : {"𝔄","ℜ","∑","∏","∫"})
            QTest::newRow(s) << QString(s) << QString("Latin Modern Math") << false << true;
    }

    void printedMathGlyphs()
    {
        QFETCH(QString,glyph); QFETCH(QString,family); QFETCH(bool,italic); QFETCH(bool,expectFormula);
        if (!QFontDatabase::families().contains(family)) QSKIP("Test font unavailable");
        QImage image(220,80,QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter p(&image);
        QFont font(family);
        font.setPixelSize(40);
        font.setItalic(italic);
        p.setFont(font);
        p.drawText(95,53,glyph);
        p.end();
        QRect ink;
        for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x)
            if (qGray(image.pixel(x,y))<175) ink |= QRect(x,y,1,1);
        QVERIFY(!ink.isEmpty());
        const auto raw=reading({word(1,1,{8,35,55,17},"Пусть"),word(1,1,ink,"у"),
                                word(1,1,{156,35,55,17},"равно")},image.size());
        QPainter prose(&image);
        prose.fillRect(raw.words.first().bounds,Qt::black);
        prose.fillRect(raw.words.last().bounds,Qt::black);
        prose.end();
        mathBackend();
        processing::OcrOptions options{.language="rus"};
        options.history=std::make_shared<processing::RecognitionRecorder>(QString(),QJsonObject{});
        auto result=processing::recognizeSmartBlocks(image,options,
            {{"line",image.rect(),"rus",false,"mixed"}},false,{},true,&raw);
        QVERIFY2(result,qPrintable(result ? QString() : result.error().message));
        QCOMPARE(result->words.size(),3);
        QCOMPARE(result->words[1].layoutBlock,expectFormula);
        if (expectFormula) {
            const QHash<QString,QString> latex{{"𝔄","\\mathfrak{A}"},{"ℜ","\\mathfrak{R}"},
                {"∑","\\sum"},{"∫","\\int"},{"∏","\\prod"}};
            const auto expected = QStringLiteral("\\(%1\\)").arg(latex.value(glyph,glyph));
            QCOMPARE(result->words[1].text,expected);
            QCOMPARE(result->words[1].alternative.text,QStringLiteral("\\(x=1\\)"));
            QCOMPARE(result->specialistSpan(1),QVector<int>{1});
            QVERIFY(!result->words[1].alternative.historyIds.isEmpty());
            auto unicode = *result;
            QCOMPARE(core::setFormulaOutput(unicode,true),0);
            QCOMPARE(unicode.specialistSpan(1),QVector<int>{1});
            auto accepted = core::WordEditor::acceptSpecialist(unicode,1,unicode.words[1].alternative.text);
            QVERIFY(accepted);
            QCOMPARE(accepted->words[1].text,QStringLiteral("x=1"));
            QCOMPARE(core::formulaSourceText(accepted->words[1]),QStringLiteral("\\(x=1\\)"));
            const auto &runs = result->recognitionHistory.runs;
            QVERIFY(std::ranges::any_of(runs,[&](const auto &run) {
                return run.engine == "math-glyph-matcher" && run.modelHash.size()==64 && run.processedCrop == ink;
            }));
        } else QCOMPARE(result->words[1].text,QStringLiteral("у"));
        QCOMPARE(result->words.first().id,raw.words.first().id);
        QCOMPARE(result->words.last().id,raw.words.last().id);
        QCOMPARE(result->words[1].sourceWordIds,raw.words[1].sourceWordIds);
    }

    void operatorCollectsScriptsAndSideOperands()
    {
        const QRect op(90,42,14,32);
        const auto raw=reading({word(1,1,{5,48,35,12},"Пусть"),word(1,1,{77,52,6,10},"2"),
            word(1,1,op,"∫"),word(1,1,{110,49,6,13},"t"),word(1,1,{170,48,40,12},"далее"),
            word(1,2,{96,31,6,6},"n"),word(1,3,{92,80,5,5},"0")},{225,105});
        // Native reading order interleaves the limits with prose elsewhere.
        // Explicit Math allows that association; Mixed keeps native prose in order.
        auto image=scan(raw);
        const QVector<core::ImageRegion> blocks{{"left",{0,0,42,105},{},false,"text"},
            {"equation",op,{},false,"math"},{"right",{155,0,70,105},{},false,"text"}};
        mathBackend();
        auto result=processing::recognizeSmartBlocks(image,{.language="rus"},blocks,false,{},true,&raw);
        QVERIFY2(result,qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result),(QStringList{"Пусть","x=1","далее"}));
        const auto &formula=result->words[1];
        for (int i : {1,2,3,5,6}) QVERIFY(formula.bounds.contains(raw.words[i].bounds));
        QCOMPARE(result->imageRegions[1].bounds,op);
        auto selected=processing::recognizeSmartBlocks(image,{.language="rus"},{blocks[1]},false,{},false,&raw);
        QVERIFY(selected);
        QCOMPARE(selected->words.size(),1);
        QCOMPARE(selected->words[0].bounds,op);
        auto mixed=processing::recognizeSmartBlocks(image,{.language="rus"},
            {{"mixed",image.rect(),{},false,"mixed"}},false,{},true,&raw);
        QVERIFY2(mixed,qPrintable(mixed ? QString() : mixed.error().message));
        QCOMPARE(text(*mixed),(QStringList{"Пусть","\\(x=1\\)","далее"}));
        for (int i : {1,2,3,5,6}) QVERIFY(mixed->words[1].bounds.contains(raw.words[i].bounds));

        const QVector<core::ImageRegion> sideBySide{{"mixed",{0,0,108,105},"rus",false,"mixed"},
            {"text",{108,0,117,105},"rus",false,"text"}};
        auto across=processing::recognizeSmartBlocks(image,{.language="rus"},sideBySide,false,{},true,&raw);
        QVERIFY(across);
        QCOMPARE(text(*across),(QStringList{"Пусть","\\(x=1\\)","далее"}));
        QVERIFY(across->words[1].bounds.contains(raw.words[3].bounds));
        QCOMPARE(across->imageRegions[1].bounds,sideBySide[1].bounds);
    }

    void misreadIntegralKeepsBothLimitsInMixedText()
    {
        const QRect upper(59,20,6,7), op(55,30,9,33), lower(54,66,11,8);
        const auto raw = reading({word(1,1,upper,"2"), word(1,2,{5,39,21,10},"т.е."),
            word(1,2,op,"{"), word(1,2,{68,40,34,11},"f(x)="), word(1,2,{107,40,9,12},"1"),
            word(1,2,{140,40,29,10},"далее"), word(1,3,lower,"р")},{180,90});
        QImage image = scan(raw);
        QPainter p(&image);
        p.fillRect(op,Qt::white);
        p.fillRect(58,30,6,3,Qt::black); p.fillRect(58,32,2,29,Qt::black);
        p.fillRect(55,60,5,3,Qt::black); p.end();
        const QVector<core::ImageRegion> regions{{"line",{1,28,175,49},{},false,"mixed"}};
        mathBackend();
        const auto result = processing::recognizeSmartBlocks(image,{.language="rus"},regions,false,{},true,&raw);
        QVERIFY(result);
        QCOMPARE(text(*result),(QStringList{"т.е.","\\(x=1\\)","далее"}));
        QVERIFY(result->words[1].bounds.contains(upper) && result->words[1].bounds.contains(lower));
        QVERIFY(!result->words[1].bounds.intersects(raw.words[1].bounds));
    }

    void acceptedInlineCropReadsOnceInProseOrder()
    {
        auto raw=reading({word(1,1,{82,8,8,6},"n"),word(1,2,{10,20,42,10},"Before"),
                          word(1,2,{75,22,25,3},"-"),word(1,2,{130,20,45,10},"after"),
                          word(1,3,{82,32,8,8},"2")},{200,58});
        auto image=scan(raw);
        const QRect crop(70,7,34,34);
        QVector<core::ImageRegion> regions{{"left",{0,16,62,20},{},false,"text"},
            {"expression",crop,{},false,"math",false,"inline_formula"},
            {"right",{120,16,72,20},{},false,"text"}};
        mathBackend();
        auto result=processing::recognizeSmartBlocks(image,{.language="eng"},regions,false,{},false,&raw);
        QVERIFY(result);
        QCOMPARE(text(*result),(QStringList{"Before","\\(x=1\\)","after"}));
        QCOMPARE(result->words[1].bounds,crop);
        QCOMPARE(result->words[1].line,result->words[0].line);
    }

    void textAndFormulaScheduling_data() {
        QTest::addColumn<bool>("eager");
        QTest::addColumn<bool>("parallel");
        QTest::addColumn<bool>("formulaFails");
        QTest::newRow("automatic-page") << true << true << false;
        QTest::newRow("typed-blocks") << false << true << false;
        QTest::newRow("formula-failure-keeps-text") << true << true << true;
        QTest::newRow("shared-engine-serialized") << true << false << false;
    }
    void textAndFormulaScheduling() {
        QFETCH(bool, eager); QFETCH(bool, parallel); QFETCH(bool, formulaFails);
        const auto raw = reading({word(1, 1, {10, 10, 55, 12}, "Before"),
            word(2, 1, {10, 50, 55, 14}, "x=0")}, {120, 90});
        const QVector<core::ImageRegion> regions = {
            {"text", {0, 0, 100, 35}, "rus", false, "text", false},
            {"formula", {0, 40, 100, 40}, "rus", false, "math", false}};
        ConcurrentState state; state.overlap = parallel; state.failFormula = formulaFails;
        ConcurrentEngine textEngine(state, raw, false), formulaEngine(state, raw, true);
        QStringList warnings;
        auto result = processing::recognizeSmartBlocks(scan(raw), {.language="rus"}, regions, false, {},
            true, nullptr, &warnings, &textEngine, &formulaEngine, eager, parallel);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(state.peak.load(), parallel ? 2 : 1);
        QCOMPARE(state.formulaCalls.load(), 1);
        QCOMPARE(text(*result), (QStringList{"Before", formulaFails ? "x=0" : "x=1"}));
        QCOMPARE(result->words[0].id, raw.words[0].id);
        QCOMPARE(warnings.isEmpty(), !formulaFails);
    }
    void textFailureStopsInFlightFormulaWork() {
        const auto raw = reading({word(1, 1, {10, 50, 55, 14}, "x=0")}, {120, 90});
        ConcurrentState state; state.failText = true; state.stallFormula = true;
        ConcurrentEngine textEngine(state, raw, false), formulaEngine(state, raw, true);
        auto result = processing::recognizeSmartBlocks(scan(raw), {.language="rus"},
            {{"formula", {0, 40, 100, 40}, "rus", false, "math", false}}, false, {},
            true, nullptr, nullptr, &textEngine, &formulaEngine, true, true);
        QVERIFY(!result); QCOMPARE(result.error().message, "Test recognition failure");
        QCOMPARE(state.peak.load(), 2); QCOMPARE(state.active.load(), 0);
    }
    void earlyAndTextDependentFormulasKeepTheirOrder() {
        const auto raw = reading({word(1, 1, {10, 10, 55, 12}, "Before"),
            word(2, 1, {10, 50, 55, 14}, "x=0"), word(3, 1, {10, 100, 55, 14}, "y=2"),
            word(4, 1, {10, 150, 55, 14}, "z=3")}, {120, 190});
        const QVector<core::ImageRegion> regions = {
            {"first", {0, 40, 100, 40}, "rus", false, "math", false},
            {"last", {0, 140, 100, 40}, "rus", false, "math", false}};
        ConcurrentState state;
        ConcurrentEngine textEngine(state, raw, false), formulaEngine(state, raw, true);
        auto result = processing::recognizeSmartBlocks(scan(raw), {.language="rus"}, regions, false, {},
            true, nullptr, nullptr, &textEngine, &formulaEngine, true, true);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), (QStringList{"Before", "x=1", "x=1", "x=1"}));
        QCOMPARE(state.formulaCalls.load(), 3);
        for (int i = 1; i < 4; ++i) {
            QCOMPARE(result->words[i].bounds, raw.words[i].bounds);
            QCOMPARE(result->words[i].sourceWordIds, raw.words[i].sourceWordIds);
        }
    }
    void cancellationStopsBothEngines() {
        const auto raw = reading({word(1, 1, {10, 50, 55, 14}, "x=0")}, {120, 90});
        ConcurrentState state; state.stallFormula = state.stallText = true;
        ConcurrentEngine textEngine(state, raw, false), formulaEngine(state, raw, true);
        std::stop_source stop;
        auto pending = std::async(std::launch::async, [&] {
            return processing::recognizeSmartBlocks(scan(raw), {.language="rus"},
                {{"formula", {0, 40, 100, 40}, "rus", false, "math", false}}, false, stop.get_token(),
                true, nullptr, nullptr, &textEngine, &formulaEngine, true, true);
        });
        QElapsedTimer timer; timer.start();
        while (state.started < 2 && timer.elapsed() < 3000) QTest::qWait(1);
        const bool bothStarted = state.started == 2;
        stop.request_stop();
        QVERIFY(bothStarted);
        QVERIFY(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        const auto result = pending.get();
        QVERIFY(!result); QVERIFY(result.error().cancelled); QCOMPARE(state.active.load(), 0);
    }
    void initTestCase()
    {
        QVERIFY(temporary.isValid());
        // Only missing text needs a new OCR call; the raw page is supplied below.
        QFile script(temporary.filePath("tesseract"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\n"
            "printf 'level\\tpage_num\\tblock_num\\tpar_num\\tline_num\\tword_num\\tleft\\ttop\\twidth\\theight\\tconf\\ttext\\n'\n"
            "printf '5\\t1\\t1\\t1\\t1\\t1\\t8\\t8\\t10\\t10\\t95\\tRecovered\\n'\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        originalPath = qgetenv("PATH");
        qputenv("PATH", QFile::encodeName(temporary.path()) + ':' + originalPath);
        originalDataHome = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", QFile::encodeName(temporary.path()));
        mathBackend();
    }

    void cleanupTestCase() { qputenv("PATH", originalPath); qputenv("XDG_DATA_HOME", originalDataHome); }

    void overlappingLinesUseRawParagraphs()
    {
        const auto raw = reading({word(1, 1, {100, 10, 80, 14}, "Заголовок"),
            word(1, 1, {250, 11, 100, 12}, "страницы"),
            word(2, 1, {10, 45, 90, 14}, "Первая"),
            word(2, 1, {150, 42, 80, 28}, "строка"),
            word(2, 2, {10, 65, 50, 12}, "Вторая"),
            word(2, 2, {100, 61, 80, 19}, "строка")}, {400, 130});
        // The tall box overlaps the next line; a missed third line forces recovery.
        const auto result = processing::recognizeSmartBlocks(scan(raw, {10, 96, 60, 14}),
            {.language = "rus"}, {}, false, {}, true, &raw);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), (QStringList{"Заголовок", "страницы", "Первая", "строка", "Вторая", "строка", "Recovered"}));
        QCOMPARE(result->imageRegions.size(), 2);
        QVERIFY(result->imageRegions[1].bounds.contains(QPoint(10, 96)));
        QCOMPARE(result->words[2].id, raw.words[2].id);
        QVERIFY(result->words.last().line != result->words[5].line);
        core::OcrDocument book;
        book.sourcePath = "reading-order.png";
        book.replacePage(*result);
        QVERIFY(book.plainText().contains("Первая строка\nВторая строка\nRecovered"));
        core::OcrDocument restored;
        QString error;
        QVERIFY2(core::OcrDocument::fromJson(book.toJson(), &restored, &error), qPrintable(error));
        QCOMPARE(restored.plainText(), book.plainText());
    }

    void recoveredWordsStayInTheirColumn()
    {
        const auto raw = reading({word(1, 1, {10, 10, 80, 12}, "LeftFirst"),
            word(1, 2, {10, 60, 80, 12}, "LeftLast"),
            word(2, 1, {300, 10, 80, 12}, "RightFirst"),
            word(2, 2, {300, 60, 80, 12}, "RightLast")}, {420, 100});
        const auto result = processing::recognizeSmartBlocks(scan(raw, {10, 35, 50, 12}),
            {.language = "rus"}, {}, false, {}, true, &raw);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), (QStringList{"LeftFirst", "Recovered", "LeftLast", "RightFirst", "RightLast"}));
        QCOMPARE(result->imageRegions.size(), 2);
        QVERIFY(result->imageRegions[0].bounds.right() < result->imageRegions[1].bounds.left());
    }

    void recoveredWordKeepsItsRawLine()
    {
        const auto raw = reading({word(1, 1, {10, 20, 30, 12}, "Before"),
                                  word(1, 1, {200, 20, 60, 12}, "After")}, {300, 70});
        const auto result = processing::recognizeSmartBlocks(scan(raw, {100, 20, 30, 12}),
            {.language = "rus"}, {}, false, {}, true, &raw);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), (QStringList{"Before", "Recovered", "After"}));
        QCOMPARE(result->words[1].line, result->words[0].line);
        QCOMPARE(result->words[1].line, result->words[2].line);
        QCOMPARE(result->words[2].id, raw.words[1].id);
    }

    void explicitBlocksAndExclusionsAreRetained()
    {
        const auto raw = reading({word(1, 1, {10, 10, 80, 12}, "Included"),
                                  word(2, 1, {300, 10, 80, 12}, "Excluded")}, {420, 80});
        const core::ImageRegion selected{"selected", {0, 0, 120, 60}, "rus", false, "text", false};
        const auto result = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"},
            {selected}, false, {}, false, &raw);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), QStringList{"Included"});
        QCOMPARE(result->imageRegions.size(), 1);
        QCOMPARE(result->imageRegions[0].id, selected.id);
        QCOMPARE(result->imageRegions[0].bounds, selected.bounds);

        auto excluded = selected;
        excluded.notText = true;
        const auto masked = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"},
            {excluded}, false, {}, true, &raw);
        QVERIFY2(masked, qPrintable(masked ? QString() : masked.error().message));
        QCOMPARE(text(*masked), QStringList{"Excluded"});
        QVERIFY(masked->imageRegions[0].notText);
    }

    void aiPicturesAreExcludedAndKeepTheirMetadata()
    {
        const auto raw = reading({word(1, 1, {10, 10, 80, 12}, "Prose"),
                                  word(2, 1, {300, 10, 80, 12}, "Diagram")}, {420, 80});
        const core::ImageRegion picture{"chart", {280, 0, 140, 60}, {}, true, "picture", true, "chart", .95};
        const auto result = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"},
            {picture}, false, {}, true, &raw);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), QStringList{"Prose"});
        QCOMPARE(result->imageRegions[0].id, picture.id);
        QCOMPARE(result->imageRegions[0].content, "picture");
        QCOMPARE(result->imageRegions[0].detectedLabel, "chart");
        QCOMPARE(result->imageRegions[0].detectionScore, .95);
    }

    void splittingProseUsesRawParagraphs()
    {
        const auto raw = reading({word(1, 1, {10, 10, 70, 12}, "First"),
            word(1, 2, {30, 40, 70, 12}, "Indented"),
            word(2, 1, {10, 80, 70, 12}, "Second")}, {200, 110});
        const auto result = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"},
            {}, true, {}, true, &raw);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QVERIFY(result->words.isEmpty());
        QCOMPARE(result->imageRegions.size(), 2);
        QCOMPARE(result->imageRegions[0].bounds, raw.regions[0].paragraphs[0].bounds);
        QCOMPARE(result->imageRegions[1].bounds, raw.regions[1].paragraphs[0].bounds);
    }

    void imageHintSeparatesDisplayEquationFromProse()
    {
        auto raw = reading({word(1, 1, {10, 30, 110, 18}, "уравнения"),
            word(1, 1, {125, 30, 35, 18}, "(6):"),
            word(2, 1, {10, 142, 100, 18}, "Следующий")}, {300, 180});
        const QRect hint(108, 52, 80, 62);
        raw.imageRegions.append({"native-image", hint, {}, false, {}, true});
        QImage image = scan(raw);
        {
            QPainter painter(&image);
            painter.fillRect(140, 56, 15, 20, Qt::black);
            painter.fillRect(108, 82, 80, 2, Qt::black);
            painter.fillRect(140, 90, 15, 20, Qt::black);
            painter.fillRect(75, 79, 15, 14, Qt::black);
        }
        const auto split = processing::recognizeSmartBlocks(image, {.language = "rus"},
            {}, true, {}, true, &raw);
        QVERIFY2(split, qPrintable(split ? QString() : split.error().message));
        QCOMPARE(split->imageRegions.size(), 3);
        QCOMPARE(split->imageRegions[0].content, "text");
        QCOMPARE(split->imageRegions[1].content, "math");
        QCOMPARE(split->imageRegions[2].content, "text");
        const QRect formula = split->imageRegions[1].bounds;
        QCOMPARE(formula, QRect(75, 56, 113, 54));
        QVERIFY(!formula.intersects(raw.regions[0].bounds));

        const core::ImageRegion manual{"manual", image.rect(), "rus", false, "text"};
        const auto textOnly = processing::recognizeSmartBlocks(image, {.language = "rus"},
            {manual}, true, {}, false, &raw);
        QVERIFY2(textOnly, qPrintable(textOnly ? QString() : textOnly.error().message));
        QCOMPARE(textOnly->imageRegions.size(), 1);
        QCOMPARE(textOnly->imageRegions[0].content, "text");
    }

    void mixedPagePreservesColumnOrderAndSourceIds()
    {
        mathBackend();
        const auto raw = reading({word(1, 1, {10, 10, 80, 12}, "LeftFirst"),
            word(1, 2, {10, 40, 50, 12}, "x=0"),
            word(1, 3, {10, 70, 80, 12}, "LeftLast"),
            word(2, 1, {300, 10, 80, 12}, "RightFirst"),
            word(2, 2, {300, 70, 80, 12}, "RightLast")}, {420, 100});
        const auto result = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"}, {}, false, {}, true, &raw);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), (QStringList{"LeftFirst", "x=1", "LeftLast", "RightFirst", "RightLast"}));
        QVERIFY(result->words[1].layoutBlock);
        QCOMPARE(result->words[1].sourceWordIds, raw.words[1].sourceWordIds);
        QCOMPARE(result->words[1].readingEngine, "rapid-latex-ocr");
        for (int i : {0, 2, 3, 4}) {
            QCOMPARE(result->words[i].id, raw.words[i].id);
            QCOMPARE(result->words[i].line, raw.words[i].line);
        }
        core::OcrDocument document, restored;
        document.sourcePath = "mixed.png";
        document.replacePage(*result);
        QString error;
        QVERIFY2(core::OcrDocument::fromJson(document.toJson(), &restored, &error), qPrintable(error));
        QCOMPARE(restored.plainText(), document.plainText());
    }

    void failedCropKeepsNativeTextAndSuccessfulEquations()
    {
        mathBackend(true);
        const auto raw = reading({word(1, 1, {10, 10, 80, 12}, "Before"),
            word(1, 2, {10, 40, 50, 12}, "x=0"),
            word(1, 3, {10, 70, 50, 12}, "y=2"),
            word(1, 4, {10, 100, 80, 12}, "After")}, {200, 140});
        QStringList warnings;
        const auto result = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"}, {}, false, {}, true, &raw, &warnings);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), (QStringList{"Before", "x=1", "y=2", "After"}));
        QVERIFY(result->words[1].layoutBlock);
        QVERIFY(!result->words[2].layoutBlock);
        QCOMPARE(result->words[2].id, raw.words[2].id);
        QVERIFY(warnings.join(' ').contains("bad crop"));
        mathBackend();
    }

    void indexRangesAndFootnotesStayText()
    {
        const auto raw = reading({word(1, 1, {10, 10, 60, 12}, "Клейн"),
            word(1, 1, {90, 10, 100, 12}, "304–307"),
            word(1, 2, {10, 40, 100, 12}, "211-213"),
            word(1, 3, {10, 70, 18, 12}, "2*"),
            word(1, 4, {10, 100, 30, 12}, "1*).")}, {220, 140});
        const auto result = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"}, {}, false, {}, true, &raw);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), text(raw));
        for (const auto &word : result->words) QVERIFY(!word.layoutBlock);
    }

    void axisFragmentIsNotAutomaticallyTranscribed()
    {
        const auto raw = reading({word(1, 1, {10, 10, 30, 12}, "x=0")}, {300, 60});
        auto image = scan(raw);
        { QPainter painter(&image); painter.fillRect(10, 25, 260, 2, Qt::black); }
        const auto result = processing::recognizeSmartBlocks(image, {.language = "rus"}, {}, false, {}, true, &raw);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), text(raw));
        QVERIFY(!result->words[0].layoutBlock);
    }

    void displayEquationDoesNotMergeWithAdjacentProse()
    {
        mathBackend();
        const auto raw = reading({word(1, 1, {10, 10, 100, 12}, "Before"),
            word(2, 1, {10, 28, 50, 14}, "x=0"),
            word(3, 1, {10, 48, 100, 12}, "After")}, {150, 90});
        const auto result = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"}, {}, false, {}, true, &raw);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), (QStringList{"Before", "x=1", "After"}));
        QCOMPARE(result->words[1].bounds, raw.words[1].bounds);
        QCOMPARE(result->imageRegions.size(), 3);
        QCOMPARE(result->imageRegions[1].content, "math");
        QCOMPARE(result->regions[1].type, "equation");
    }

    void proseJoinedToMathPunctuationKeepsItsNativeReading()
    {
        mathBackend();
        const auto raw = reading({word(1, 1, {10, 10, 60, 12}, "флюксиях»*),"),
            word(1, 2, {10, 40, 60, 12}, "тоа+ь"),
            word(1, 3, {10, 70, 50, 12}, "x=0")}, {150, 110});
        const auto result = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"}, {}, false, {}, true, &raw);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), (QStringList{"флюксиях»*),", "тоа+ь", "x=1"}));
        QCOMPARE(result->words[0].id, raw.words[0].id);
        QCOMPARE(result->words[1].id, raw.words[1].id);
    }

    void compactCyrillicAssignmentsAreMathInsideProse()
    {
        for (const auto &tokens : {QStringList{"рз=2,"}, QStringList{"р3=2,"},
                                   QStringList{"рз", "=", "2,"}}) {
            QVector<core::OcrWord> words{word(1, 1, {10, 10, 55, 16}, "справа:")};
            int x = 75;
            for (const auto &token : tokens) {
                words.append(word(1, 1, {x, 10, 30, 16}, token));
                x += 35;
            }
            words.append(word(1, 1, {x+10, 10, 50, 16}, "слово"));
            const auto raw = reading(words, {320, 60});
            const auto result = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"}, {}, false, {}, true, &raw);
            QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
            QCOMPARE(text(*result), (QStringList{"справа:", "\\(x=1\\)", "слово"}));
            QVERIFY(result->words[1].layoutBlock);
            QCOMPARE(result->words[0].id, raw.words[0].id);
            QCOMPARE(result->words[2].id, raw.words.last().id);
            QStringList sources;
            for (int i = 1; i+1 < raw.words.size(); ++i) sources += raw.words[i].sourceWordIds;
            QCOMPARE(result->words[1].sourceWordIds, sources);
        }
    }

    void appliedLayoutMathOverridesMisreadCyrillicText()
    {
        mathBackend();
        const auto raw = reading({word(1, 1, {10, 10, 30, 12}, "ро"),
            word(1, 1, {50, 10, 20, 12}, "за"), word(1, 1, {80, 10, 30, 12}, "3,14285")}, {120, 50});
        const core::ImageRegion block{"ai-formula", {0, 0, 120, 50}, "rus", false,
            "math", true, "display_formula", .95};
        const auto result = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"},
            {block}, false, {}, false, &raw);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), QStringList{"x=1"});
        QVERIFY(result->words[0].layoutBlock);
        QStringList sources;
        for (const auto &word : raw.words) sources += word.sourceWordIds;
        QCOMPARE(result->words[0].sourceWordIds, sources);
        QCOMPARE(result->imageRegions[0].id, block.id);
        QVERIFY(result->imageRegions[0].autoDetected);
        QCOMPARE(result->imageRegions[0].detectedLabel, block.detectedLabel);
    }

    void displayFormulaUsesWholeCropAcrossWhitespaceAndFractionLines()
    {
        mathBackend();
        const auto raw = reading({word(1, 1, {20, 20, 8, 8}, "p"),
                                  word(1, 2, {20, 43, 8, 8}, "q"),
                                  word(2, 1, {182, 28, 70, 12}, "(k=0,1)")}, {280, 75});
        const core::ImageRegion block{"equation", {8, 12, 255, 48}, "rus", false,
                                      "math", true, "display_formula", .89};
        const auto result = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"},
            {block}, false, {}, false, &raw);
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result), QStringList{"x=1"});
        QCOMPARE(result->words.first().bounds, block.bounds);
        QCOMPARE(result->words.first().sourceWordIds.size(), 3);
    }

    void acceptedFormulaReplacesInterleavedTokensWithoutDroppingProse()
    {
        mathBackend();
        const auto raw=reading({word(1,1,{10,10,55,12},"Before"),
            word(2,1,{110,50,30,14},"ро"),word(3,1,{10,100,60,14},"Outside"),
            word(2,1,{170,50,30,14},"ра"),word(4,1,{10,130,55,12},"After")},{240,160});
        const core::ImageRegion block{"ai-row",{100,40,120,40},"rus",false,"math",true,"display_formula",.95};
        const auto result=processing::recognizeSmartBlocks(scan(raw),{.language="rus"},{block},false,{},true,&raw);
        QVERIFY2(result,qPrintable(result ? QString() : result.error().message));
        QCOMPARE(text(*result),(QStringList{"Before","x=1","Outside","After"}));
        QCOMPARE(result->words[1].sourceWordIds,raw.words[1].sourceWordIds+raw.words[3].sourceWordIds);
        QCOMPARE(result->words[2].id,raw.words[2].id);
    }

    void automaticMathOutlineCannotOverrideProseProtection()
    {
        mathBackend();
        const auto raw = reading({word(1, 1, {10, 10, 60, 12}, "флюксиях»*),")}, {100, 50});
        core::ImageRegion block{"auto-math", {0, 0, 100, 50}, "rus", false, "math", true};
        const auto automatic = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"},
            {block}, false, {}, false, &raw);
        QVERIFY2(automatic, qPrintable(automatic ? QString() : automatic.error().message));
        QCOMPARE(text(*automatic), text(raw));
        QVERIFY(!automatic->words[0].layoutBlock);
        block.autoDetected = false;
        const auto manual = processing::recognizeSmartBlocks(scan(raw), {.language = "rus"},
            {block}, false, {}, false, &raw);
        QVERIFY2(manual, qPrintable(manual ? QString() : manual.error().message));
        QCOMPARE(text(*manual), QStringList{"x=1"});
        QVERIFY(manual->words[0].layoutBlock);
    }
};

QTEST_MAIN(SmartBlocksTests)
#include "smartblocks_tests.moc"
