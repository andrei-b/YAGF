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
    explicit FixtureTextEngine(core::OcrPage output, QString name = "tesseract", bool boxes = true)
        : output(std::move(output)), name(std::move(name)), boxes(boxes) {}
    QString id() const override { return name; }
    processing::EngineCapabilities capabilities() const override { return {.supportsRegions = true, .supportsWordBoxes = boxes}; }
    std::expected<core::OcrPage, processing::OcrError> recognize(
        const QImage &, const processing::OcrOptions &, std::stop_token) override { return output; }
private:
    core::OcrPage output;
    QString name;
    bool boxes;
};

class SpellingFormulasTests : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QByteArray dataHome, enchantConfig, originalPath;

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

    void textResponse(const QString &text = "слово", int confidence = 96, const QStringList &failLanguages = {})
    {
        write(temporary.filePath("text-response.json"), QJsonDocument(QJsonObject{
            {"text", text}, {"confidence", confidence}, {"fail_languages", QJsonArray::fromStringList(failLanguages)}}).toJson());
    }

    static core::OcrPage formula(const QString &language = "rus")
    {
        auto p = page({word("\\mathrm{cnobo}", 10)}, language);
        p.words[0].layoutBlock = true;
        p.words[0].readingEngine = "rapid-latex-ocr";
        p.regions[0].type = "equation";
        p.imageRegions = {{"math", p.words[0].bounds, {}, false, "math", true}};
        return p;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(temporary.isValid());
        dataHome = qgetenv("XDG_DATA_HOME");
        enchantConfig = qgetenv("ENCHANT_CONFIG_DIR");
        originalPath = qgetenv("PATH");
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
        const QString tesseract = temporary.filePath("tesseract");
        write(tesseract, "#!/usr/bin/python3\nimport json,os,sys\n"
            "root=os.environ['XDG_DATA_HOME']\n"
            "reply=json.load(open(root+'/text-response.json'))\n"
            "with open(root+'/text-calls.jsonl','a') as calls: calls.write(json.dumps(sys.argv[1:])+'\\n')\n"
            "if sys.argv[sys.argv.index('-l')+1] in reply['fail_languages']: sys.exit(1)\n"
            "print('level\\tpage_num\\tblock_num\\tpar_num\\tline_num\\tword_num\\tleft\\ttop\\twidth\\theight\\tconf\\ttext')\n"
            "for line_no,line in enumerate(reply['text'].splitlines(),1):\n"
            " x=8\n"
            " for n,word in enumerate(line.split(),1):\n"
            "  width=min(30,max(2,len(word)*3))\n"
            "  print(f'5\\t1\\t1\\t1\\t{line_no}\\t{n}\\t{x}\\t{8+(line_no-1)*12}\\t{width}\\t8\\t{reply[\"confidence\"]}\\t{word}')\n"
            "  x+=width+3\n");
        QVERIFY(QFile::setPermissions(tesseract, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        qputenv("PATH", QFile::encodeName(temporary.path())+':'+originalPath);
    }

    void cleanupTestCase()
    {
        if (dataHome.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", dataHome);
        if (enchantConfig.isNull()) qunsetenv("ENCHANT_CONFIG_DIR"); else qputenv("ENCHANT_CONFIG_DIR", enchantConfig);
        qputenv("PATH", originalPath);
    }

    void init()
    {
        response(); textResponse();
        QFile::remove(temporary.filePath("crops.json"));
        QFile::remove(temporary.filePath("text-calls.jsonl"));
    }

    void formulaIsRetriedAsTextAndAcceptanceRestoresTextMetadata()
    {
        auto p = formula();
        const auto before = p;
        const auto result = processing::collectFormulaTextReadings(p, image(p), "rus", {});
        QVERIFY(result); QVERIFY(result->isEmpty());
        QCOMPARE(p.words[0].text, before.words[0].text);
        QCOMPARE(p.words[0].bounds, before.words[0].bounds);
        QVERIFY(p.words[0].layoutBlock);
        QCOMPARE(p.words[0].alternative.text, "слово");
        QVERIFY(!p.words[0].alternative.formula);
        QCOMPARE(p.specialistSpan(0), QVector<int>{0});
        core::OcrDocument doc, restored;
        doc.sourcePath = "scan.png"; doc.replacePage(p);
        QString error;
        QVERIFY2(core::OcrDocument::fromJson(doc.toJson(), &restored, &error), qPrintable(error));
        const auto kept = core::WordEditor::acceptSpecialist(*restored.page(1), 0, before.words[0].text);
        QVERIFY(kept); QVERIFY(kept->words[0].layoutBlock);
        QCOMPARE(kept->regions[0].type, "equation");
        const auto accepted = core::WordEditor::acceptSpecialist(*restored.page(1), 0, "слово");
        QVERIFY(accepted); QVERIFY(!accepted->words[0].layoutBlock);
        QCOMPARE(accepted->words[0].text, "слово");
        QCOMPARE(accepted->words[0].bounds, before.words[0].bounds);
        QCOMPARE(accepted->words[0].sourceWordIds, before.words[0].sourceWordIds);
        QCOMPARE(accepted->words[0].readingEngine, "tesseract");
        QCOMPARE(accepted->regions[0].type, "text");
        QCOMPARE(accepted->imageRegions[0].content, "text");
        doc.replacePage(*accepted);
        QCOMPARE(doc.plainText(), "слово");
        QVERIFY(core::buildVerificationQueue(doc).items.isEmpty());
    }

    void reverseRetriesRequireReliableDictionarySupportedProse()
    {
        for (const auto &text : {"qzxqzxqzx", "x=2", "x", "Hello qzxqzxqzx", "\\mathrm{Hello}", ""}) {
            textResponse(text);
            auto p = formula("eng");
            QVERIFY(processing::collectFormulaTextReadings(p, image(p), "eng", {}));
            QVERIFY2(p.words[0].alternative.text.isEmpty(), text);
        }
        textResponse("Hello", 30);
        auto p = formula("eng");
        QVERIFY(processing::collectFormulaTextReadings(p, image(p), "eng", {}));
        QVERIFY(p.words[0].alternative.text.isEmpty());
        textResponse("Hello world");
        p = formula("eng");
        QVERIFY(processing::collectFormulaTextReadings(p, image(p), "eng", {}));
        QCOMPARE(p.words[0].alternative.text, "Hello world");
        const auto accepted = core::WordEditor::acceptSpecialist(p, 0, "Hello world");
        QVERIFY(accepted); QCOMPARE(accepted->words.size(), 2);
        for (const auto &w : accepted->words) { QVERIFY(!w.layoutBlock); QVERIFY(w.reviewed); }
        QCOMPARE(accepted->words[0].bounds.united(accepted->words[1].bounds), p.words[0].bounds);
    }

    void reverseUsesBlockLanguagesFallbackAndHyphenatedWords()
    {
        for (const auto &language : {"rus", "eng", "deu", "rus+eng"}) {
            auto p = formula(language);
            QVERIFY(processing::collectFormulaTextReadings(p, image(p), language, {}));
            QCOMPARE(p.words[0].alternative.text, "слово");
        }
        auto p = formula("auto");
        QVERIFY(processing::collectFormulaTextReadings(p, image(p), "rus", {}));
        QCOMPARE(p.words[0].alternative.text, "слово");
        p = formula("zz_ZZ");
        p.imageRegions[0].language = "rus";
        QVERIFY(processing::collectFormulaTextReadings(p, image(p), "eng", {}));
        QCOMPARE(p.words[0].alternative.text, "слово");
        textResponse("пере-\nнос");
        p = formula(); p.words[0].bounds.setHeight(30);
        QVERIFY(processing::collectFormulaTextReadings(p, image(p), "rus", {}));
        QCOMPARE(p.words[0].alternative.text, "пере-\nнос");
    }

    void reverseRespectsReviewAndExplicitMathAndHandlesFailure()
    {
        auto p = formula(); p.words[0].reviewed = true;
        QVERIFY(processing::collectFormulaTextReadings(p, image(p), "rus", {}));
        QVERIFY(p.words[0].alternative.text.isEmpty());
        p = formula(); p.imageRegions[0].autoDetected = false;
        QVERIFY(processing::collectFormulaTextReadings(p, image(p), "rus", {}));
        QVERIFY(p.words[0].alternative.text.isEmpty());
        p = formula("zz_ZZ");
        QVERIFY(processing::collectFormulaTextReadings(p, image(p), "zz_ZZ", {}));
        QVERIFY(p.words[0].alternative.text.isEmpty());
        QVERIFY(!QFile::exists(temporary.filePath("text-calls.jsonl")));
        p = formula(); textResponse("Hello", 96, {"rus"});
        const auto result = processing::collectFormulaTextReadings(p, image(p), "rus", {});
        QVERIFY(result); QVERIFY(!result->isEmpty());
        QVERIFY(p.words[0].alternative.text.isEmpty());
        std::stop_source stop; stop.request_stop();
        const auto cancelled = processing::collectFormulaTextReadings(p, image(p), "rus", stop.get_token());
        QVERIFY(!cancelled); QVERIFY(cancelled.error().cancelled);
    }

    void reversePipelineAlsoRunsForMathOnlyEngines()
    {
        auto raw = formula("auto"); raw.imageRegions.clear();
        const QString path = temporary.filePath("math-scan.png");
        QVERIFY(image(raw).save(path));
        processing::DocumentProcessor processor;
        processor.setEngine(std::make_unique<FixtureTextEngine>(raw, "rapid-latex-ocr", false));
        core::OcrPage recognized;
        connect(&processor, &processing::DocumentProcessor::recognized, this,
            [&](core::OcrPage result, const QPoint &, const QRect &) { recognized = std::move(result); });
        QSignalSpy failed(&processor, &processing::DocumentProcessor::failed);
        processor.recognize(path, "rus", 1);
        QTRY_VERIFY_WITH_TIMEOUT(!processor.isBusy(), 5000);
        QVERIFY(failed.isEmpty()); QCOMPARE(recognized.words.size(), 1);
        QCOMPARE(recognized.words[0].alternative.text, "слово");
    }

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

    void shortAssignmentsBypassDictionaryIdentifierExclusions()
    {
        response("p_{3}=2,");
        for (const auto &tokens : {QStringList{"рз=2,"}, QStringList{"р3=2,"}, QStringList{"p3=2,"},
                                  QStringList{"рз", "=", "2,"}}) {
            QVector<core::OcrWord> words{word("русский", 10)};
            int x = 80;
            for (const auto &token : tokens) { words.append(word(token, x)); x += 45; }
            words.append(word("слово", x+20));
            auto p = page(words);
            QVERIFY(processing::collectSpellingFormulaReadings(p, image(p), {}));
            QCOMPARE(crops().size(), 1);
            QCOMPARE(p.words[1].alternative.text, "p_{3}=2,");
            QCOMPARE(p.specialistSpan(1).size(), tokens.size());
            QVERIFY(p.words[0].alternative.text.isEmpty());
            QVERIFY(p.words.last().alternative.text.isEmpty());
        }
        auto p = page({word("р3=2", 10)}, "zz_ZZ");
        QVERIFY(processing::collectSpellingFormulaReadings(p, image(p), {}));
        QVERIFY(p.words[0].alternative.formula);
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
