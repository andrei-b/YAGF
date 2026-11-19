#include "smartblocks.h"

#include <QFile>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

using namespace yagf;

class SmartBlocksTests : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QByteArray originalPath;

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
    }

    void cleanupTestCase() { qputenv("PATH", originalPath); }

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
};

QTEST_GUILESS_MAIN(SmartBlocksTests)
#include "smartblocks_tests.moc"
