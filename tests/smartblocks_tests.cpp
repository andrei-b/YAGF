#include "smartblocks.h"

#include <QFile>
#include <QDir>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

using namespace yagf;

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

QTEST_GUILESS_MAIN(SmartBlocksTests)
#include "smartblocks_tests.moc"
