#include "spellingformulas.h"
#include "documentprocessor.h"
#include "spellchecker.h"
#include "verificationqueue.h"
#include "wordeditor.h"
#include "wordspelling.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

using namespace yagf;

class FixtureTextEngine : public processing::IOcrEngine {
public:
    explicit FixtureTextEngine(core::OcrPage output) : output(std::move(output)) {}
    QString id() const override { return "tesseract"; }
    processing::EngineCapabilities capabilities() const override { return {.supportsRegions = true, .supportsWordBoxes = true}; }
    std::expected<core::OcrPage, processing::OcrError> recognize(
        const QImage &, const processing::OcrOptions &, std::stop_token) override { return output; }
private:
    core::OcrPage output;
};

class SpellingFormulasTests : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QByteArray dataHome, enchantConfig;

    void write(const QString &path, const QByteArray &data)
    {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(data), data.size());
    }

    static core::OcrWord word(const QString &text, int x, int y = 10, int line = 1)
    {
        return {1, 1, 1, line, {x, y, 40, 12}, text, text, 96};
    }

    static core::OcrPage page(QVector<core::OcrWord> words, const QString &language = "rus")
    {
        core::OcrPage p;
        p.id = "page-1";
        p.engineId = "tesseract";
        p.language = language;
        p.width = p.sourceWidth = 500;
        p.height = p.sourceHeight = 160;
        p.words = std::move(words);
        p.rebuildHierarchy();
        return p;
    }

    static QImage image(const core::OcrPage &p)
    {
        QImage scan(p.width, p.height, QImage::Format_RGB32);
        scan.fill(Qt::white);
        QPainter painter(&scan);
        for (const auto &w : p.words) painter.fillRect(w.bounds, Qt::black);
        return scan;
    }

    void response(const QString &text = "x^{2}=1", bool failSecond = false)
    {
        QJsonObject reply{{"text", text}, {"fail_second", failSecond}};
        write(temporary.filePath("response.json"), QJsonDocument(reply).toJson());
    }

    QJsonArray crops()
    {
        QFile file(temporary.filePath("crops.json"));
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).array();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(temporary.isValid());
        dataHome = qgetenv("XDG_DATA_HOME");
        enchantConfig = qgetenv("ENCHANT_CONFIG_DIR");
        qputenv("XDG_DATA_HOME", QFile::encodeName(temporary.path()));
        qputenv("ENCHANT_CONFIG_DIR", QFile::encodeName(temporary.filePath("enchant")));
        write(temporary.filePath("enchant/enchant.ordering"), "*:hunspell\n");
        for (const auto &code : {"ru_RU", "en_US", "de_DE", "sv_SE"}) {
            write(temporary.filePath(QString("enchant/hunspell/%1.aff").arg(code)), "SET UTF-8\n");
            write(temporary.filePath(QString("enchant/hunspell/%1.dic").arg(code)),
                QString("8\nрусский\nслово\nперенос\nHello\nworld\nrecognition\nWort\nZeilenumbruch\n").toUtf8());
        }
        core::SpellChecker checker;
        checker.setLanguages({"rus+eng+deu"});
        if (checker.activeDictionaries().size() != 3) QSKIP("Requires the optional Enchant Hunspell provider");
        QVERIFY(!checker.isMisspelled("перенос"));
        QVERIFY(checker.isMisspelled("абвгджзщщ"));
        const QString python = temporary.filePath("yagf2/math-ocr/venv/bin/python");
        write(python, "#!/usr/bin/python3\nimport json,os,sys\n"
            "root=os.environ['XDG_DATA_HOME']\n"
            "boxes=json.load(open(sys.argv[3]))\n"
            "json.dump(boxes,open(root+'/crops.json','w'))\n"
            "reply=json.load(open(root+'/response.json'))\nresult=[]\n"
            "for i,(l,t,r,b) in enumerate(boxes):\n"
            " entry={'box':[l,t,r-l,b-t]}\n"
            " entry.update({'error':'bad crop'} if reply['fail_second'] and i==1 else {'text':reply['text']})\n"
            " result.append(entry)\nprint(json.dumps(result))\n");
        QVERIFY(QFile::setPermissions(python, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    }

    void cleanupTestCase()
    {
        if (dataHome.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", dataHome);
        if (enchantConfig.isNull()) qunsetenv("ENCHANT_CONFIG_DIR"); else qputenv("ENCHANT_CONFIG_DIR", enchantConfig);
    }

    void init() { response(); QFile::remove(temporary.filePath("crops.json")); }

    void russianCandidatesKeepNativeTextAndAcceptSourceLinkedFormula()
    {
        auto p = page({word("русский", 10), word("абвгджзщщ", 70), word("щзжгфц", 115), word("слово", 190)});
        const auto before = p;
        const auto result = processing::collectSpellingFormulaReadings(p, image(p), {});
        QVERIFY(result); QVERIFY(result->isEmpty());
        QCOMPARE(crops().size(), 1);
        QCOMPARE(p.words.size(), before.words.size());
        QCOMPARE(p.specialistSpan(1), (QVector<int>{1, 2}));
        QVERIFY(p.words[1].alternative.formula);
        QCOMPARE(p.words[1].alternative.text, "x^{2}=1");
        for (int i = 0; i < p.words.size(); ++i) {
            QCOMPARE(p.words[i].text, before.words[i].text);
            QCOMPARE(p.words[i].id, before.words[i].id);
            QCOMPARE(p.words[i].bounds, before.words[i].bounds);
        }
        QVERIFY(p.words[0].alternative.text.isEmpty());
        core::OcrDocument document, restored;
        document.sourcePath = "scan.png";
        document.replacePage(p);
        QString error;
        QVERIFY2(core::OcrDocument::fromJson(document.toJson(), &restored, &error), qPrintable(error));
        QCOMPARE(restored.page(1)->specialistSpan(1), (QVector<int>{1, 2}));
        const auto accepted = core::WordEditor::acceptSpecialist(*restored.page(1), 1, p.words[1].alternative.text);
        QVERIFY(accepted); QCOMPARE(accepted->words.size(), 3);
        QVERIFY(accepted->words[1].layoutBlock);
        QCOMPARE(accepted->words[1].sourceBoxes.size(), 2);
        const auto queue = core::buildVerificationQueue(document);
        QVERIFY(std::ranges::any_of(queue.items, [](const auto &item) {
            return (item.reasons & core::ConflictingReading) && (item.reasons & core::MathReading);
        }));
    }

    void languagesAndMixedLanguageBlocks()
    {
        for (const auto &language : {"eng", "deu", "swe", "rus+eng+deu"}) {
            auto p = page({word("Hello", 10), word("qzxqzxqzx", 70)}, language);
            QVERIFY(processing::collectSpellingFormulaReadings(p, image(p), {}));
            QVERIFY(p.words[0].alternative.text.isEmpty());
            QVERIFY(p.words[1].alternative.formula);
        }
        auto p = page({word("qzxqzxqzx", 10), word("абвгджзщщ", 100)}, "eng");
        p.imageRegions = {{"ru", {90, 0, 70, 40}, "rus"}};
        QVERIFY(processing::collectSpellingFormulaReadings(p, image(p), {}));
        QCOMPARE(crops().size(), 2);
        for (const auto &w : p.words) QVERIFY(w.alternative.formula);
        p = page({word("qzxqzxqzx", 10)}, "zz_ZZ");
        QVERIFY(processing::collectSpellingFormulaReadings(p, image(p), {}));
        QVERIFY(p.words[0].alternative.text.isEmpty());
        p.language = "auto";
        QVERIFY(processing::collectSpellingFormulaReadings(p, image(p), {}));
        QVERIFY(p.words[0].alternative.text.isEmpty());
    }

    void lineBreaksAreJoinedForSpellingAndNeverRetriedAsFragments()
    {
        for (const auto &parts : {QStringList{"пере-", "нос", "rus"},
                                  QStringList{"recog‐", "nition", "eng"},
                                  QStringList{"Zeilen\u00ad", "umbruch", "deu"}}) {
            auto p = page({word(parts[0], 120), word(parts[1], 10, 35, 2)}, parts[2]);
            const auto spelling = core::wordSpelling(p);
            QCOMPARE(spelling.continuations, (QSet<int>{0, 1}));
            QCOMPARE(spelling.tokens[0], spelling.tokens[1]);
            QVERIFY(processing::collectSpellingFormulaReadings(p, image(p), {}));
            QVERIFY(crops().isEmpty());
            core::SpellChecker checker; checker.setLanguages({parts[2]});
            core::OcrDocument document; document.sourcePath = "scan.png"; document.replacePage(p);
            const auto queue = core::buildVerificationQueue(document, [&](const auto &, const auto &, const auto &token) {
                return checker.isMisspelled(token);
            });
            QVERIFY(queue.items.isEmpty());
        }
        auto p = page({word("пере", 80), word("-", 123), word("нос", 10, 35, 2)});
        QCOMPARE(core::wordSpelling(p).continuations, (QSet<int>{0, 1, 2}));
        QVERIFY(processing::collectSpellingFormulaReadings(p, image(p), {}));
        QVERIFY(crops().isEmpty());
        p.words[2].block = 2; // A different column or paragraph is not a continuation.
        QVERIFY(core::wordSpelling(p).continuations.isEmpty());
    }

    void textReadingsAndFailedCropsKeepNativeOcr()
    {
        for (const auto &reading : {"some text", "\\text{word+word}", "\\mathrm{word}"}) {
            response(reading);
            auto p = page({word("qzxqzxqzx", 10)}, "eng");
            QVERIFY(processing::collectSpellingFormulaReadings(p, image(p), {}));
            QVERIFY(p.words[0].alternative.text.isEmpty());
            QCOMPARE(p.words[0].text, "qzxqzxqzx");
        }
        response("x=1", true);
        auto p = page({word("qzxqzxqzx", 10), word("qzxqzxqzx", 10, 50, 2)}, "eng");
        const auto result = processing::collectSpellingFormulaReadings(p, image(p), {});
        QVERIFY(result); QVERIFY(result->join(' ').contains("bad crop"));
        QVERIFY(p.words[0].alternative.formula);
        QVERIFY(p.words[1].alternative.text.isEmpty());
        std::stop_source stop; stop.request_stop();
        const auto cancelled = processing::collectSpellingFormulaReadings(p, image(p), stop.get_token());
        QVERIFY(!cancelled); QVERIFY(cancelled.error().cancelled);
    }

    void respectsReviewedWordsExplicitTextAndExistingReadings()
    {
        auto p = page({word("qzxqzxqzx", 10), word("qzxqzxqzx", 70), word("qzxqzxqzx", 130), word("qzxqzxqzx", 190)}, "eng");
        p.words[0].reviewed = true;
        p.words[1].alternative = {"existing", "original alternative", {p.words[1].text, p.words[2].text}, false};
        p.imageRegions = {{"text", {185, 0, 70, 40}, "eng", false, "text"}};
        QVERIFY(processing::collectSpellingFormulaReadings(p, image(p), {}));
        QVERIFY(crops().isEmpty());
        QCOMPARE(p.words[1].alternative.engine, "existing");
        p.imageRegions[0].notText = true;
        QVERIFY(processing::collectSpellingFormulaReadings(p, image(p), {}));
        QVERIFY(crops().isEmpty());
    }

    void boundsWorkOnVeryNoisyPages()
    {
        auto p = page({}, "eng");
        p.height = p.sourceHeight = 2800;
        for (int i = 0; i < 130; ++i) p.words.append(word("qzxqzxqzx", 10, 10+20*i, i+1));
        p.rebuildHierarchy();
        const auto result = processing::collectSpellingFormulaReadings(p, image(p), {});
        QVERIFY(result); QVERIFY(result->join(' ').contains("128"));
        QCOMPARE(crops().size(), 128);
        QVERIFY(p.words[127].alternative.formula);
        QVERIFY(p.words[128].alternative.text.isEmpty());
        QCOMPARE(p.words[129].text, "qzxqzxqzx");
    }

    void recognitionPipelineRetriesAndRawModeBypasses()
    {
        const auto raw = page({word("русский", 10), word("абвгджзщщ", 70)});
        const QString path = temporary.filePath("scan.png");
        QVERIFY(image(raw).save(path));
        core::OcrDocument document;
        document.sourcePath = path;
        processing::DocumentProcessor processor(nullptr, &document);
        processor.setEngine(std::make_unique<FixtureTextEngine>(raw));
        core::OcrPage recognized;
        connect(&processor, &processing::DocumentProcessor::recognized, this,
            [&](core::OcrPage result, const QPoint &, const QRect &) { recognized = std::move(result); });
        QSignalSpy failed(&processor, &processing::DocumentProcessor::failed);
        processor.recognize(path, "rus", 1);
        QTRY_VERIFY_WITH_TIMEOUT(!processor.isBusy(), 5000);
        QVERIFY(failed.isEmpty());
        QCOMPARE(recognized.words.size(), 2);
        QVERIFY(recognized.words[1].alternative.formula);
        QCOMPARE(crops().size(), 1);
        QFile::remove(temporary.filePath("crops.json"));
        document.useRawOcrOutput = true;
        processor.recognize(path, "rus", 1);
        QTRY_VERIFY_WITH_TIMEOUT(!processor.isBusy(), 5000);
        QVERIFY(failed.isEmpty());
        QCOMPARE(recognized.words.size(), 2);
        QVERIFY(recognized.words[1].alternative.text.isEmpty());
        QVERIFY(crops().isEmpty());
    }
};

QTEST_GUILESS_MAIN(SpellingFormulasTests)
#include "spellingformulas_tests.moc"
