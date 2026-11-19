#include "exportpage.h"
#include "documentprocessor.h"
#include "pdfexporter.h"
#include "mathocrengine.h"
#include "smartblocks.h"
#include "projectfile.h"
#include "remoteocrengine.h"
#include "scientificbookexporter.h"
#include "tesseractengine.h"
#include "wordeditor.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace yagf;

namespace {
core::OcrDocument referenceBook()
{
    core::OcrDocument book;
    book.sourcePath = QStringLiteral(MATH_FIXTURE_DIR "/equations.pdf");
    book.totalPages = 8;
    book.rendering.dpi = 300;
    return book;
}

QRect inkBounds(const QImage &image)
{
    QRect bounds;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qGray(image.pixel(x, y)) < 100) bounds |= QRect(x, y, 1, 1);
    return bounds;
}

// Poppler can shift an antialiased edge slightly when rendering an exported scan.
// Count ink pixels in one image with no nearby ink in the other.
double unmatchedInk(const QImage &from, const QImage &to)
{
    int ink = 0, missed = 0;
    for (int y = 0; y < from.height(); ++y) {
        for (int x = 0; x < from.width(); ++x) {
            if (qGray(from.pixel(x, y)) >= 100) continue;
            ++ink;
            const auto nearby = QRect(x-2, y-2, 5, 5).intersected(to.rect());
            bool found = false;
            for (int j = nearby.top(); !found && j <= nearby.bottom(); ++j)
                for (int i = nearby.left(); !found && i <= nearby.right(); ++i)
                    found = qGray(to.pixel(i, j)) < 150;
            if (!found) ++missed;
        }
    }
    return ink ? double(missed)/ink : 1.0;
}
}

class MathRecognitionTests : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QStringList references;
    QList<QImage> scans;

private slots:
    void initTestCase()
    {
        QVERIFY(temporary.isValid());
        if (QStandardPaths::findExecutable("pdftoppm").isEmpty()) QSKIP("Requires pdftoppm");
        QFile tex(QStringLiteral(MATH_FIXTURE_DIR "/equations.tex"));
        QVERIFY(tex.open(QIODevice::ReadOnly));
        // The typesetter input is also the ground truth.
        for (const auto &line : tex.readAll().split('\n'))
            if (line.startsWith("\\sample{") && line.endsWith('}'))
                references.append(QString::fromUtf8(line.mid(8, line.size()-9)));
        const auto book = referenceBook();
        QCOMPARE(references.size(), book.totalPages);
        for (int page = 1; page <= book.totalPages; ++page) {
            auto image = processing::readExportPage(book, page, temporary, false);
            QVERIFY2(image, qPrintable(image ? QString() : image.error().error));
            QVERIFY(!inkBounds(*image).isEmpty());
            scans.append(std::move(*image));
        }
    }

    void localOcr_data()
    {
        QTest::addColumn<int>("page");
        QTest::addColumn<QStringList>("otherReadings");
        QTest::addColumn<bool>("knownLimitation");
        QTest::newRow("addition") << 1 << QStringList{} << false;
        QTest::newRow("minus") << 2 << QStringList{"19−7=12"} << true;
        QTest::newRow("inequality") << 3 << QStringList{} << false;
        QTest::newRow("exponent") << 4 << QStringList{"x²+1=10", "x^2+1=10"} << true;
        QTest::newRow("subscripts") << 5 << QStringList{"a₁+a₂=7", "a_1+a_2=7"} << true;
        QTest::newRow("stacked-fractions") << 6 << QStringList{"1/2+1/4=3/4"} << true;
        QTest::newRow("radical") << 7 << QStringList{"sqrt(x+1)=3", "√(x+1)=3"} << true;
        QTest::newRow("summation") << 8 << QStringList{} << true;
    }

    void localOcr()
    {
        QFETCH(int, page);
        QFETCH(QStringList, otherReadings);
        QFETCH(bool, knownLimitation);
        if (QStandardPaths::findExecutable("tesseract").isEmpty()) QSKIP("Requires Tesseract");
        QProcess models;
        models.start("tesseract", {"--list-langs"});
        QVERIFY(models.waitForFinished());
        QCOMPARE(models.exitCode(), 0);
        if (!models.readAllStandardOutput().split('\n').contains("eng")) QSKIP("Requires the English model");
        processing::TesseractEngine engine;
        const auto recognized = engine.recognize(scans[page-1], {.language="eng", .pageNumber=page}, {});
        QVERIFY2(recognized, qPrintable(recognized ? QString() : recognized.error().message));
        core::OcrDocument result;
        result.pages = {*recognized};
        const QString actual = result.plainText().simplified().remove(' ');
        const QString expected = QString(references.at(page-1)).remove(' ');
        const bool match = actual == expected || otherReadings.contains(actual);
        const QString diagnostic = QString("Page %1: expected %2, read [%3]").arg(page).arg(expected, actual);
        if (knownLimitation && !match && !qEnvironmentVariableIntValue("YAGF_STRICT_MATH_OCR"))
            QEXPECT_FAIL("", qPrintable(diagnostic), Continue);
        QVERIFY2(match, qPrintable(diagnostic));
    }

    void localMathPage()
    {
        if (processing::MathOcrEngine::pythonPath().isEmpty()) QSKIP("Requires the local math OCR runtime");
        const QImage image(QStringLiteral(MATH_FIXTURE_DIR "/../../../examples/math-review/equations.png"));
        QVERIFY(!image.isNull());
        processing::MathOcrEngine engine;
        const auto page = engine.recognize(image, {.pageNumber = 3}, {});
        QVERIFY2(page, qPrintable(page ? QString() : page.error().message));
        QCOMPARE(page->words.size(), 2);
        QCOMPARE(page->words[0].text, QStringLiteral("x^{2}+1=10"));
        QCOMPARE(page->words[1].text, QStringLiteral(R"(\frac{1}{2}+\frac{1}{4}=\frac{3}{4})"));
        QVERIFY(page->words[0].bounds.bottom() < page->words[1].bounds.top());
        QCOMPARE(page->regions.size(), 2);
        for (const auto &region : page->regions) QCOMPARE(region.type, "equation");
    }

    void russianTextAndMath()
    {
        QProcess models;
        models.start("tesseract", {"--list-langs"});
        if (!models.waitForFinished() || !models.readAllStandardOutput().split('\n').contains("rus"))
            QSKIP("Requires the Russian Tesseract model");
        const QImage image(QStringLiteral(MATH_FIXTURE_DIR "/../../../examples/math-review/russian-math.png"));
        QVERIFY(!image.isNull());
        const processing::OcrOptions options{.language = "rus", .pageNumber = 1};
        const auto split = processing::recognizeSmartBlocks(image, options, {}, true, {});
        QVERIFY2(split, qPrintable(split ? QString() : split.error().message));
        QStringList kinds;
        for (const auto &block : split->imageRegions) kinds.append(block.content);
        QCOMPARE(kinds, (QStringList{"text", "math", "mixed"}));
        QVERIFY(split->words.isEmpty());
        const auto &paragraph = split->imageRegions.first();
        QVERIFY(paragraph.autoDetected);
        const auto prose = processing::recognizeSmartBlocks(image, options, {paragraph}, false, {}, false);
        QVERIFY2(prose, qPrintable(prose ? QString() : prose.error().message));
        QCOMPARE(prose->words.size(), 6);
        for (const auto &word : prose->words) {
            QVERIFY(!word.layoutBlock);
            QVERIFY(paragraph.bounds.contains(word.bounds.center()));
        }
        core::OcrDocument book;
        book.sourcePath = "russian-math.png";
        book.replacePage(*split);
        core::OcrDocument loaded;
        QString error;
        QVERIFY2(core::OcrDocument::fromJson(book.toJson(), &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.pages.first().imageRegions.last().content, "mixed");
        // Start as the Recognize button does, without any saved blocks or split step.
        core::OcrDocument fresh;
        fresh.sourcePath = QStringLiteral(MATH_FIXTURE_DIR "/../../../examples/math-review/russian-math.png");
        processing::DocumentProcessor processor(nullptr, &fresh);
        QSignalSpy success(&processor, &processing::DocumentProcessor::recognized);
        QSignalSpy failure(&processor, &processing::DocumentProcessor::failed);
        QSignalSpy warnings(&processor, &processing::DocumentProcessor::specialistWarning);
        processor.recognize(fresh.sourcePath, "rus", 1);
        QTRY_VERIFY_WITH_TIMEOUT(!success.isEmpty() || !failure.isEmpty(), 60000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure.isEmpty() ? QString() : failure.first().first().toString()));
        const auto read = success.first().first().value<core::OcrPage>();
        QStringList automaticTypes;
        for (const auto &block : read.imageRegions) automaticTypes.append(block.content);
        QCOMPARE(automaticTypes, kinds);
        book.replacePage(read);
        if (processing::MathOcrEngine::pythonPath().isEmpty()) {
            QVERIFY(!warnings.isEmpty());
            QCOMPARE(read.engineId, "tesseract");
            QVERIFY(book.plainText().contains(QStringLiteral("Рассмотрим")));
            return;
        }
        QCOMPARE(book.plainText().simplified(),
            QStringLiteral(R"(Рассмотрим простой пример. Сначала запишем равенство. \frac{1}{2}+\frac{1}{4}=\frac{3}{4} Уравнение \(x^{2}+1=10\) имеет решение.)"));
        QCOMPARE(read.regions[2].type, "equation");
    }

    void liveSurya()
    {
        const auto endpoint = qEnvironmentVariable("YAGF_TEST_SURYA_URL");
        if (endpoint.isEmpty()) QSKIP("Set YAGF_TEST_SURYA_URL to test a running Surya model");
        processing::RemoteOcrEngine engine(QUrl(endpoint), qEnvironmentVariable("YAGF_OCR_TOKEN"),
                                            processing::RemoteOcrEngine::Provider::Surya);
        QStringList mismatches;
        for (int i = 0; i < scans.size(); ++i) {
            const auto page = engine.recognize(scans[i], {.language = "auto", .pageNumber = i + 1}, {});
            QVERIFY2(page, qPrintable(page ? QString() : page.error().message));
            core::OcrDocument result;
            result.pages = {*page};
            QString actual = result.plainText().simplified().remove(' ');
            if ((actual.startsWith("\\[") && actual.endsWith("\\]")) ||
                (actual.startsWith("\\(") && actual.endsWith("\\)")))
                actual = actual.mid(2, actual.size() - 4);
            const auto expected = QString(references[i]).remove(' ');
            if (actual != expected)
                mismatches.append(QString("Page %1: expected %2, read [%3]").arg(i + 1).arg(expected, actual));
        }
        QVERIFY2(mismatches.isEmpty(), qPrintable(mismatches.join('\n')));
    }

    void formattedMathWithoutTex()
    {
        const auto &scan = scans.first();
        core::OcrDocument book;
        book.sourcePath = temporary.filePath("source-equation.png");
        QVERIFY(scan.save(book.sourcePath));
        core::OcrPage page;
        page.width = page.sourceWidth = scan.width();
        page.height = page.sourceHeight = scan.height();
        const QString latex = references.first();
        page.words.append({1, 1, 1, 1, inkBounds(scan), latex, latex, 95});
        page.rebuildHierarchy();
        page.regions.first().type = "equation";
        book.replacePage(page);

        using Pdf = processing::PdfExporter;
        const auto path = temporary.filePath("scanned-equation-without-tex.pdf");
        const auto toolsPath = qgetenv("PATH");
        qputenv("PATH", QFile::encodeName(temporary.path()));
        const bool available = Pdf::hasPdfLatex();
        const auto result = Pdf::write(book, path, false, false, {}, Pdf::Mode::FormattedText, Pdf::FormulaStyle::Typeset);
        qputenv("PATH", toolsPath);
        QVERIFY(!available);
        QVERIFY2(result.status == processing::ExportResult::Status::Success, qPrintable(result.error));
        core::OcrDocument exported;
        exported.sourcePath = path;
        auto rendered = processing::readExportPage(exported, 1, temporary, false);
        QVERIFY2(rendered, qPrintable(rendered ? QString() : rendered.error().error));
        QVERIFY(unmatchedInk(scan, *rendered) < .01);
        QVERIFY(unmatchedInk(*rendered, scan) < .01);
        if (!QStandardPaths::findExecutable("pdftotext").isEmpty()) {
            QProcess extract;
            extract.start("pdftotext", {"-raw", path, "-"});
            QVERIFY(extract.waitForFinished());
            QCOMPARE(QString::fromUtf8(extract.readAllStandardOutput()).trimmed(), latex);
        }
    }

    void typesetEditedMath()
    {
        if (QStandardPaths::findExecutable("pdflatex").isEmpty() || QStandardPaths::findExecutable("pdftotext").isEmpty())
            QSKIP("Requires pdflatex and Poppler");
        core::OcrDocument book;
        book.sourcePath = temporary.filePath("formula-source.png");
        QImage scan(600, 300, QImage::Format_RGB32);
        scan.fill(Qt::white); // Any visible ink in the result must come from typesetting.
        QVERIFY(scan.save(book.sourcePath));
        core::OcrPage page;
        page.number = 1;
        page.width = page.sourceWidth = scan.width();
        page.height = page.sourceHeight = scan.height();
        const QString display = R"(\begin{pmatrix}x_1&\sqrt{2}\\1&\frac{1}{2}\end{pmatrix})";
        const QString inlineMath = R"(\(x^{3}+1=9\))";
        const QString prose = R"(For \(x=2\), the value is \(x^3=8\).)";
        const QStringList readings{display, inlineMath, prose};
        const QList<QRect> boxes{{40,30,180,80}, {100,145,110,28}, {18,210,480,35}};
        for (int i = 0; i < readings.size(); ++i) {
            core::OcrWord word{1, i+1, 1, 1, boxes[i], "old", readings[i], 90};
            word.layoutBlock = true;
            page.words.append(word);
        }
        page.rebuildHierarchy();
        page.regions[0].type = "equation";
        book.replacePage(page);
        using Pdf = processing::PdfExporter;
        const auto name = temporary.filePath("typeset.pdf");
        const auto written = Pdf::write(book, name, false, false, {}, Pdf::Mode::FormattedText, Pdf::FormulaStyle::Typeset);
        QVERIFY2(written.status == processing::ExportResult::Status::Success, qPrintable(written.error));
        QProcess extract;
        extract.start("pdftotext", {"-raw", name, "-"});
        QVERIFY(extract.waitForFinished());
        QCOMPARE(QString::fromUtf8(extract.readAllStandardOutput()).simplified(), readings.join(' '));
        if (!QStandardPaths::findExecutable("pdffonts").isEmpty()) {
            QProcess fonts;
            fonts.start("pdffonts", {name});
            QVERIFY(fonts.waitForFinished());
            QVERIFY(fonts.readAllStandardOutput().contains("CMMI")); // Native Computer Modern math font.
        }
        auto rendered = book;
        rendered.sourcePath = name;
        auto img = processing::readExportPage(rendered, 1, temporary, false);
        QVERIFY(img);
        for (const auto &box : boxes) QVERIFY(!inkBounds(img->copy(box)).isEmpty());
        QFile previous(name);
        QVERIFY(previous.open(QIODevice::ReadOnly));
        const auto saved = previous.readAll();
        previous.close();
        book.pages[0].words[0].text = R"(\unknownformat{x})";
        const auto failed = Pdf::write(book, name, false, false, {}, Pdf::Mode::FormattedText, Pdf::FormulaStyle::Typeset);
        QCOMPARE(failed.status, processing::ExportResult::Status::Error);
        QVERIFY(failed.error.contains("page 1") && failed.error.contains("unknownformat"));
        QVERIFY(previous.open(QIODevice::ReadOnly));
        QCOMPARE(previous.readAll(), saved);

        // The default formatted export can preserve even an unsupported formula.
        // With a blank source, both display and inline formula crops must stay blank.
        const auto scanned = temporary.filePath("scanned-formulas.pdf");
        const auto kept = Pdf::write(book, scanned, false, false, {}, Pdf::Mode::FormattedText);
        QVERIFY2(kept.status == processing::ExportResult::Status::Success, qPrintable(kept.error));
        rendered.sourcePath = scanned;
        img = processing::readExportPage(rendered, 1, temporary, false);
        QVERIFY(img);
        for (const auto &box : boxes) QVERIFY(inkBounds(img->copy(box)).isEmpty());
        extract.start("pdftotext", {"-raw", scanned, "-"});
        QVERIFY(extract.waitForFinished());
        QCOMPARE(QString::fromUtf8(extract.readAllStandardOutput()).simplified(),
                 QStringList({book.pages[0].words[0].text, inlineMath, prose}).join(' '));
    }

    void reviewedExports()
    {
        // Exercise the normal review operation independently of installed OCR models.
        auto book = referenceBook();
        QStringList expected;
        for (int i = 0; i < scans.size(); ++i) {
            core::OcrPage page;
            page.number = i+1;
            page.renderDpi = 300;
            page.width = page.sourceWidth = scans[i].width();
            page.height = page.sourceHeight = scans[i].height();
            const QString baseline = "unreadable";
            const QString latex = "\\[" + references[i] + "\\]";
            page.words.append({page.number, 1, 1, 1, inkBounds(scans[i]), baseline, baseline, 35});
            page.rebuildHierarchy();
            page.words[0].alternative = {"surya-remote", latex, {baseline}, true};
            // Correct a wrong operator in the proposed reading on the subtraction page.
            if (i == 1) page.words[0].alternative.text.replace('-', '+');
            auto accepted = core::WordEditor::acceptSpecialist(page, 0, latex);
            QVERIFY2(accepted, qPrintable(accepted ? QString() : accepted.error()));
            QCOMPARE(accepted->regions.first().type, "equation");
            QCOMPARE(accepted->words.first().readingEngine, i == 1 ? "" : "surya-remote");
            book.replacePage(std::move(*accepted));
            expected.append(latex);
        }
        const auto path = temporary.filePath("reviewed.yagf.json");
        QString error;
        QVERIFY2(core::ProjectFile::save(path, book, &error), qPrintable(error));
        core::OcrDocument restored;
        QVERIFY2(core::ProjectFile::load(path, &restored, &error), qPrintable(error));
        QCOMPARE(restored.plainText(), expected.join("\n\n\f\n\n"));
        const auto jsonPages = restored.toJson(false).value("pages").toArray();
        const QString scientific = temporary.filePath("book.html");
        const auto htmlResult = processing::ScientificBookExporter::write(restored, scientific, false);
        QVERIFY2(htmlResult.status == processing::ExportResult::Status::Success, qPrintable(htmlResult.error));
        QFile file(scientific);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString html = QString::fromUtf8(file.readAll());
        const auto layout = restored.layoutHtml();
        for (int i = 0; i < expected.size(); ++i) {
            QCOMPARE(jsonPages.at(i).toObject()["words"].toArray().first().toObject()["text"].toString(), expected[i]);
            const auto escaped = expected[i].toHtmlEscaped();
            QVERIFY(html.contains("<pre class=\"equation\">" + escaped + "</pre>"));
            QVERIFY(layout.contains(">" + escaped + "</span>"));
        }
        if (QStandardPaths::findExecutable("pdftotext").isEmpty()) QSKIP("PDF text checks require pdftotext");
        using Pdf = processing::PdfExporter;
        for (const auto &[mode, style] : {std::pair{Pdf::Mode::SearchableScan, Pdf::FormulaStyle::Scanned},
                                          std::pair{Pdf::Mode::FormattedText, Pdf::FormulaStyle::Scanned},
                                          std::pair{Pdf::Mode::FormattedText, Pdf::FormulaStyle::Typeset}}) {
            if (style == Pdf::FormulaStyle::Typeset && QStandardPaths::findExecutable("pdflatex").isEmpty())
                QSKIP("Formula typesetting requires pdflatex");
            const auto pdf = temporary.filePath("reviewed.pdf");
            const auto written = Pdf::write(restored, pdf, false, false, {}, mode, style);
            QVERIFY2(written.status == processing::ExportResult::Status::Success, qPrintable(written.error));
            QProcess extract;
            extract.start("pdftotext", {"-raw", pdf, "-"});
            QVERIFY(extract.waitForFinished());
            QCOMPARE(extract.exitCode(), 0);
            const auto pages = QString::fromUtf8(extract.readAllStandardOutput()).split('\f');
            QCOMPARE(pages.size(), expected.size()+1);
            auto exported = referenceBook();
            exported.sourcePath = pdf;
            if (style == Pdf::FormulaStyle::Typeset && !QStandardPaths::findExecutable("gs").isEmpty()) {
                // Strip every raster image. All eight equations must still be visible.
                exported.sourcePath = temporary.filePath("vector-only.pdf");
                QCOMPARE(QProcess::execute("gs", {"-q", "-dSAFER", "-dBATCH", "-dNOPAUSE", "-dFILTERIMAGE",
                    "-sDEVICE=pdfwrite", "-sOutputFile=" + exported.sourcePath, pdf}), 0);
            }
            for (int i = 0; i < expected.size(); ++i) {
                QCOMPARE(pages[i].trimmed(), expected[i]);
                const auto image = processing::readExportPage(exported, i+1, temporary, false);
                QVERIFY2(image, qPrintable(image ? QString() : image.error().error));
                if (style == Pdf::FormulaStyle::Scanned) {
                    QVERIFY(unmatchedInk(scans[i], *image) < .01);
                    QVERIFY(unmatchedInk(*image, scans[i]) < .01);
                } else {
                    const auto target = inkBounds(scans[i]);
                    const auto rendered = inkBounds(*image);
                    QVERIFY2(!rendered.isEmpty() && target.adjusted(-3,-3,3,3).contains(rendered),
                             qPrintable(QString("Misplaced typeset equation on page %1").arg(i+1)));
                    QVERIFY(rendered.width() > target.width() * 0.75);
                    QVERIFY(rendered.height() > target.height() * 0.75);
                }
            }
        }
    }
};

QTEST_MAIN(MathRecognitionTests)
#include "mathrecognition_tests.moc"
