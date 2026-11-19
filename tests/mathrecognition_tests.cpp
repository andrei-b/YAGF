#include "exportpage.h"
#include "pdfexporter.h"
#include "mathocrengine.h"
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
        for (auto mode : {Pdf::Mode::SearchableScan, Pdf::Mode::FormattedText}) {
            const auto pdf = temporary.filePath("reviewed.pdf");
            const auto written = Pdf::write(restored, pdf, false, false, {}, mode);
            QVERIFY2(written.status == processing::ExportResult::Status::Success, qPrintable(written.error));
            QProcess extract;
            extract.start("pdftotext", {"-raw", pdf, "-"});
            QVERIFY(extract.waitForFinished());
            QCOMPARE(extract.exitCode(), 0);
            const auto pages = QString::fromUtf8(extract.readAllStandardOutput()).split('\f');
            QCOMPARE(pages.size(), expected.size()+1);
            auto exported = referenceBook();
            exported.sourcePath = pdf;
            for (int i = 0; i < expected.size(); ++i) {
                QCOMPARE(pages[i].trimmed(), expected[i]);
                const auto image = processing::readExportPage(exported, i+1, temporary, false);
                QVERIFY2(image, qPrintable(image ? QString() : image.error().error));
                QVERIFY(unmatchedInk(scans[i], *image) < .01);
                QVERIFY(unmatchedInk(*image, scans[i]) < .01);
            }
        }
    }
};

QTEST_MAIN(MathRecognitionTests)
#include "mathrecognition_tests.moc"
