#include "correctionretention.h"
#include "verificationqueue.h"
#include "spellchecker.h"
#include "wordeditor.h"
#include "imageprocessor.h"
#include "pagesource.h"
#include "documentprocessor.h"
#include "pdfexporter.h"
#include "exportpage.h"
#include <QGuiApplication>
#include <QPainter>
#include <QPdfWriter>
#include <QPageSize>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>
using namespace yagf::core;
using namespace yagf::processing;

static OcrWord word(QString text, QRect box, QString id = "one")
{
    OcrWord result;
    result.id = id; result.sourceWordIds = {id};
    result.text = result.originalText = text; result.bounds = box; result.sourceBoxes = {box};
    result.block = result.paragraph = result.line = 1; result.confidence = 95;
    return result;
}
static OcrPage page()
{
    OcrPage p;
    p.width = p.sourceWidth = 600; p.height = p.sourceHeight = 800;
    p.id = "page-1"; p.engineId = "tesseract"; p.language = "eng";
    p.words = {word("modem", {20, 30, 80, 20})}; p.rebuildHierarchy();
    return p;
}
class ResolutionEngine final : public IOcrEngine {
public:
    QString id() const override { return "resolution-fixture"; }
    EngineCapabilities capabilities() const override { return {.supportsWordBoxes = true}; }
    std::expected<OcrPage, OcrError> recognize(const QImage &image, const OcrOptions &options, std::stop_token) override {
        OcrPage p; p.number = options.pageNumber;
        p.width = p.sourceWidth = image.width(); p.height = p.sourceHeight = image.height();
        p.words = {word("recognized", {10,10,60,12})}; p.words[0].page = options.pageNumber;
        return p;
    }
};
class Release21Tests : public QObject {
    Q_OBJECT
private slots:
    void retainJitterAndDpi() {
        auto previous = page();
        previous.words[0].text = "modern"; previous.words[0].reviewed = true;
        previous.words[0].bold = true; previous.words[0].fontFamily = "Liberation Serif";
        previous.imageRegions.append({"area", {10, 10, 110, 60}});
        auto fresh = rescalePageGeometry(page(), 1200, 1600, 1200, 1600, 300);
        fresh.words[0].bounds.translate(2, 1); fresh.words[0].sourceBoxes = {fresh.words[0].bounds};
        const auto report = retainCorrections(fresh, previous);
        QCOMPARE(report.retained, 1); QCOMPARE(report.pending, 0);
        QCOMPARE(fresh.words[0].text, "modern"); QVERIFY(fresh.words[0].reviewed && fresh.words[0].bold);
        QCOMPARE(fresh.words[0].fontFamily, "Liberation Serif");
        QCOMPARE(fresh.imageRegions[0].bounds, QRect(20, 20, 220, 120));
        QCOMPARE(fresh.words[0].bounds, QRect(42, 61, 160, 40));
    }
    void retainSplitMergeDeletion() {
        auto p = page(); p.words[0] = word("notebook", {20, 30, 80, 20});
        auto split = WordEditor::apply(p, {0}, {{"note"}, {"book"}}); QVERIFY(split.has_value());
        QVERIFY(split->deletedWords.isEmpty());
        auto fresh = p; auto report = retainCorrections(fresh, *split);
        QCOMPARE(report.retained, 2); QCOMPARE(fresh.words.size(), 2); QCOMPARE(fresh.words[1].text, "book");
        auto merged = WordEditor::apply(*split, {0, 1}, {{"notebook"}}); QVERIFY(merged.has_value());
        QVERIFY(merged->deletedWords.isEmpty()); fresh = p; retainCorrections(fresh, *merged);
        QCOMPARE(fresh.words.size(), 1); QVERIFY(fresh.words[0].reviewed);
        auto deleted = WordEditor::apply(p, {0}, {}); QVERIFY(deleted.has_value());
        QCOMPARE(deleted->deletedWords.size(), 1); QVERIFY(deleted->deletedWords[0].text.isEmpty());
        fresh = p; report = retainCorrections(fresh, *deleted);
        QCOMPARE(report.retained, 1); QVERIFY(fresh.words.isEmpty());
        auto rerun = p; retainCorrections(rerun, fresh); QVERIFY(rerun.words.isEmpty());
    }
    void ambiguousWorkSurvivesAndRestores() {
        auto previous = page(); previous.words[0].text = "modern"; previous.words[0].reviewed = true;
        auto fresh = page(); fresh.words[0].text = fresh.words[0].originalText = "unrelated";
        auto report = retainCorrections(fresh, previous);
        QCOMPARE(report.retained, 0); QCOMPARE(report.pending, 1);
        QCOMPARE(fresh.words[0].text, "unrelated"); QCOMPARE(fresh.pendingCorrections[0].text, "modern");
        auto next = page(); next.words[0].text = next.words[0].originalText = "unrelated";
        retainCorrections(next, fresh); QCOMPARE(next.pendingCorrections.size(), 1);
        auto restored = restorePendingCorrection(next, previous.words[0].id, "modern", {0}); QVERIFY(restored.has_value());
        QVERIFY(restored->pendingCorrections.isEmpty()); QCOMPARE(restored->words[0].text, "modern"); QVERIFY(restored->words[0].reviewed);
        QVERIFY(!restorePendingCorrection(next, "missing", "text", {0}));
        // Two equally plausible OCR boxes must not consume the correction arbitrarily.
        fresh = page(); fresh.words.append(word("modem", fresh.words[0].bounds, "duplicate"));
        report = retainCorrections(fresh, previous); QCOMPARE(report.pending, 1); QCOMPARE(fresh.words.size(), 2);
    }
    void insertedAndLargePassageCorrections() {
        auto p = page();
        auto inserted = WordEditor::apply(p, {0}, {{"new"}, {"modem"}}); QVERIFY(inserted.has_value());
        auto fresh = page(); retainCorrections(fresh, *inserted);
        QCOMPARE(fresh.pendingCorrections.size(), 1); QCOMPARE(fresh.pendingCorrections[0].text, "new");
        auto restored = restorePendingCorrection(fresh, fresh.pendingCorrections[0].id, "new", {});
        QVERIFY(restored.has_value()); QCOMPARE(restored->words.size(), 2); QVERIFY(restored->pendingCorrections.isEmpty());
        p = page(); p.words[0].originalText = p.words[0].text = QString(3000,'a') + "modem" + QString(3000,'b');
        p.words[0].layoutBlock = true;
        auto previous = p; previous.words[0].text.replace("modem", "modern"); previous.words[0].reviewed = true;
        p.words[0].text = p.words[0].originalText = QString(3000,'a') + "modern" + QString(3000,'b');
        p.words[0].text[1500] = 'c'; p.words[0].originalText = p.words[0].text;
        QCOMPARE(retainCorrections(p, previous).retained, 1);
        QCOMPARE(p.words[0].text, previous.words[0].text);
        auto deleted = WordEditor::apply(page(), {0}, {}); QVERIFY(deleted.has_value());
        fresh = page(); fresh.words[0].text = fresh.words[0].originalText = "different";
        retainCorrections(fresh, *deleted); QCOMPARE(fresh.pendingCorrections.size(), 1);
        auto deletion = restorePendingCorrection(fresh, fresh.pendingCorrections[0].id, "", {0});
        QVERIFY(deletion.has_value()); QVERIFY(deletion->words.isEmpty()); QCOMPARE(deletion->deletedWords.size(), 1);
        auto rerun = fresh; rerun.pendingCorrections.clear(); retainCorrections(rerun, *deletion);
        QVERIFY(rerun.words.isEmpty() && rerun.pendingCorrections.isEmpty());
    }
    void persistAndLoadLegacy() {
        OcrDocument doc; doc.sourcePath = "source.pdf"; doc.rendering = {400, true};
        auto p = page(); p.renderDpi = 300;
        auto correction = p.words[0]; correction.reviewed = true; correction.text = "modern";
        p.pendingCorrections = {correction}; correction.text.clear(); p.deletedWords = {correction};
        doc.replacePage(p);
        OcrDocument loaded; QString error;
        QVERIFY2(OcrDocument::fromJson(doc.toJson(), &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.toJson(), doc.toJson());
        auto json = doc.toJson(); json.remove("rendering"); auto pages = json["pages"].toArray(); auto item = pages[0].toObject();
        item.remove("render_dpi"); item.remove("deleted_words"); item.remove("pending_corrections"); pages[0] = item; json["pages"] = pages;
        QVERIFY2(OcrDocument::fromJson(json, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.rendering.dpi, 150); QCOMPARE(loaded.pages[0].renderDpi, 150); QVERIFY(loaded.pages[0].pendingCorrections.isEmpty());
        json["rendering"] = QJsonObject{{"dpi", 250}, {"adaptive", true}};
        QVERIFY(!OcrDocument::fromJson(json, &loaded, &error));
        json = doc.toJson(); pages = json["pages"].toArray(); item = pages[0].toObject(); item["render_dpi"] = 0; pages[0] = item; json["pages"] = pages;
        QVERIFY(!OcrDocument::fromJson(json, &loaded, &error));
    }
    void queueReasonsAndCompletion() {
        OcrDocument doc; doc.sourcePath = "book.pdf"; doc.totalPages = 3;
        auto p = page(); p.words[0].confidence = 40;
        p.words[0].alternative = {"second", "modern", {"modem"}, false};
        p.words.append(word("collision", {30, 30, 80, 20}, "two"));
        auto pending = word("saved", {200, 200, 50, 20}, "pending"); pending.reviewed = true; p.pendingCorrections = {pending};
        doc.replacePage(p); auto other = page(); other.number = 2; other.words[0].page = 2; other.words[0].reviewed = true; doc.replacePage(other);
        auto queue = buildVerificationQueue(doc, [](const auto &, const auto &, const QString &token) { return token == "modem"; });
        QCOMPARE(queue.items.size(), 4); QCOMPARE(queue.recognizedPages, 2); QCOMPARE(queue.completedPages, 1); QCOMPARE(queue.verifiedWords, 1);
        QCOMPARE(queue.items[0].reasons, unsigned(LowConfidence | ConflictingReading | UnknownWord | GeometryAnomaly));
        QCOMPARE(queue.items[2].reasons, unsigned(PendingCorrection)); QVERIFY(queue.items[2].pending);
        QCOMPARE(queue.items[3].page, 3); QCOMPARE(queue.items[3].reasons, unsigned(UnrecognizedPage));
        doc.pages[0].words[0].reviewed = true; doc.pages[0].words[1].reviewed = true; doc.pages[0].pendingCorrections.clear();
        queue = buildVerificationQueue(doc); QCOMPARE(queue.completedPages, 2); QCOMPARE(queue.items.size(), 1);
        SpellChecker checker; checker.setLanguages({"nonexistent_dictionary"});
        QVERIFY(!checker.isMisspelled("nonsenseword"));
    }
    void fragmentGeometryAvoidsFalseOverlap() {
        OcrDocument doc; doc.sourcePath = "page.png"; auto p = page();
        p.words[0].bounds = {10, 10, 150, 100}; p.words[0].sourceBoxes = {{10, 10, 150, 20}, {10, 90, 150, 20}};
        p.words.append(word("middle", {10, 50, 80, 20}, "two")); doc.replacePage(p);
        QVERIFY(buildVerificationQueue(doc).items.isEmpty());
        doc.pages[0].words[1].bounds = {-1, 50, 80, 20};
        QCOMPARE(buildVerificationQueue(doc).items[0].reasons, unsigned(GeometryAnomaly));
    }
    void adaptiveDpiAndCommands() {
        QImage image(600, 400, QImage::Format_RGB32); image.fill(Qt::white);
        QCOMPARE(ImageProcessor::recommendedRenderDpi(image), 150);
        for (auto [height, expected] : {std::pair{16, 150}, {12, 200}, {8, 300}, {5, 400}}) {
            image.fill(Qt::white); QPainter painter(&image);
            for (int i = 0; i < 30; ++i) painter.fillRect(10 + i * 15, 20, 5, height, Qt::black);
            painter.end(); QCOMPARE(ImageProcessor::recommendedRenderDpi(image), expected);
        }
        for (int dpi : {150, 200, 300, 400}) {
            const auto pdf = pageRenderCommand("book.pdf", 2, "output", false, dpi);
            QVERIFY(pdf.arguments.contains(QString::number(dpi)));
            const auto djvu = pageRenderCommand("book.djvu", 2, "output", false, dpi);
            QVERIFY(djvu.arguments.contains(QStringLiteral("-scale=%1").arg(dpi)));
        }
    }
    void adaptiveRenderingBatchAndExportAgree() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString source = dir.filePath("adaptive.pdf");
        { QPdfWriter writer(source); writer.setResolution(150);
          writer.setPageSize(QPageSize(QSizeF(288,384), QPageSize::Point)); writer.setPageMargins(QMarginsF(0,0,0,0));
          QPainter painter(&writer);
          for (int height : {12, 8}) {
              if (height == 8) QVERIFY(writer.newPage());
              painter.fillRect(0,0,600,800,Qt::white);
              for (int i = 0; i < 30; ++i) painter.fillRect(10 + i * 15,20,5,height,Qt::black);
          }
        }
        OcrDocument doc; doc.sourcePath = source; doc.totalPages = 2; doc.rendering.adaptive = true;
        DocumentProcessor processor(nullptr, &doc); processor.setEngine(std::make_unique<ResolutionEngine>());
        QSignalSpy opened(&processor, &DocumentProcessor::documentOpened), ready(&processor, &DocumentProcessor::pageRendered);
        QSignalSpy finished(&processor, &DocumentProcessor::batchFinished), errors(&processor, &DocumentProcessor::failed);
        processor.openDocument(source); QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, 10000);
        processor.renderDocumentPage(1); QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,10000); QCOMPARE(processor.renderedDpi(1),200);
        processor.renderDocumentPage(2); QTRY_COMPARE_WITH_TIMEOUT(ready.size(),2,10000); QCOMPARE(processor.renderedDpi(2),300);
        QTemporaryDir temporary; std::atomic_bool cancel{false};
        for (int number : {1,2}) {
            auto exported = readExportPage(doc, number, temporary, cancel); QVERIFY(exported.has_value());
            QCOMPARE(exported->size(), QImage(ready[number-1][1].toString()).size());
            QCOMPARE(qRound(exported->dotsPerMeterY() * .0254), number == 1 ? 200 : 300);
        }
        connect(&processor, &DocumentProcessor::batchPageRecognized, this, [&](OcrPage p, QSize sourceSize) {
            QCOMPARE(p.renderDpi, p.number == 1 ? 200 : 300); QCOMPARE(QSize(p.width,p.height),sourceSize); doc.replacePage(p);
        });
        processor.recognizePages({{1,"eng",{}},{2,"eng",{}}}); QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,10000);
        QCOMPARE(doc.pages.size(),2); QVERIFY(errors.isEmpty());
        // Switching to a fixed default must not alter recognized pages' saved DPI.
        doc.rendering = {400,false}; processor.renderDocumentPage(1); QTRY_COMPARE_WITH_TIMEOUT(ready.size(),3,10000);
        QCOMPARE(processor.renderedDpi(1),200);
    }
    void cancellingDpiChangeKeepsCurrentOcrResolution() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString source = dir.filePath("cancel-dpi.pdf");
        { QPdfWriter writer(source); writer.setResolution(150); writer.setPageSize(QPageSize(QSizeF(288,384),QPageSize::Point));
          writer.setPageMargins(QMarginsF(0,0,0,0)); QPainter painter(&writer); painter.drawText(20,40,"Current page"); }
        OcrDocument doc; doc.sourcePath = source;
        DocumentProcessor processor(nullptr, &doc); processor.setEngine(std::make_unique<ResolutionEngine>());
        QSignalSpy opened(&processor, &DocumentProcessor::documentOpened), ready(&processor, &DocumentProcessor::pageRendered);
        QSignalSpy stopped(&processor, &DocumentProcessor::cancelled);
        processor.openDocument(source); QTRY_COMPARE_WITH_TIMEOUT(opened.size(),1,10000);
        processor.renderDocumentPage(1); QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,10000);
        doc.replacePage(page());
        processor.renderDocumentPage(1,400); processor.cancel(); QTRY_COMPARE_WITH_TIMEOUT(stopped.size(),1,10000);
        QCOMPARE(processor.renderedDpi(1),150); QCOMPARE(ready.size(),1);
        std::optional<OcrPage> recognized;
        connect(&processor,&DocumentProcessor::recognized,this,[&](OcrPage p,const QPoint &,const QRect &) { recognized = p; });
        processor.recognize(ready[0][1].toString(),"eng",1); QTRY_VERIFY_WITH_TIMEOUT(recognized.has_value(),10000);
        QCOMPARE(recognized->renderDpi,150); QCOMPARE(recognized->width,600);
        processor.renderDocumentPage(1,400); QTRY_COMPARE_WITH_TIMEOUT(ready.size(),2,10000); QCOMPARE(processor.renderedDpi(1),400);
        recognized.reset(); processor.recognize(ready[1][1].toString(),"eng",1); QTRY_VERIFY_WITH_TIMEOUT(recognized.has_value(),10000);
        QCOMPARE(recognized->renderDpi,400); QCOMPARE(recognized->width,1600);
    }
    void mixedDpiPdfAndRendering() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString source = dir.filePath("source.pdf");
        { QPdfWriter writer(source); writer.setResolution(150); writer.setPageSize(QPageSize(QSizeF(288,384), QPageSize::Point));
          writer.setPageMargins(QMarginsF(0,0,0,0)); QPainter painter(&writer);
          painter.fillRect(0, 0, 600, 800, Qt::white); painter.drawText(20, 40, "First");
          QVERIFY(writer.newPage()); painter.fillRect(0,0,600,800,Qt::white); painter.drawText(20,40,"Second"); }
        OcrDocument doc; doc.sourcePath = source; doc.totalPages = 2;
        auto first = page(); first.words[0].text = "First"; doc.replacePage(first);
        auto second = rescalePageGeometry(page(),1200,1600,1200,1600,300); second.number = 2; second.words[0].page = 2; second.words[0].text = "Second"; doc.replacePage(second);
        std::atomic_bool cancel{false};
        const QString output = dir.filePath("mixed.pdf");
        auto result = PdfExporter::write(doc, output, false, cancel);
        QVERIFY2(result.status == ExportResult::Status::Success, qPrintable(result.error));
        QProcess text; text.start("pdftotext", {"-bbox", output, "-"}); QVERIFY(text.waitForFinished(10000)); QCOMPARE(text.exitCode(),0);
        QXmlStreamReader xml(text.readAllStandardOutput()); int count = 0, words = 0;
        while (!xml.atEnd()) {
            xml.readNext(); if (!xml.isStartElement()) continue;
            if (xml.name() == QLatin1String("page")) { ++count; QVERIFY(std::abs(xml.attributes().value("width").toDouble() - 288) < 1); QVERIFY(std::abs(xml.attributes().value("height").toDouble() - 384) < 1); }
            if (xml.name() == QLatin1String("word")) { ++words; QVERIFY(xml.attributes().value("xMin").toDouble() < 20); }
        }
        QCOMPARE(count, 2); QCOMPARE(words, 2);
        DocumentProcessor processor(nullptr, &doc); QSignalSpy opened(&processor, &DocumentProcessor::documentOpened);
        QSignalSpy ready(&processor, &DocumentProcessor::pageRendered); QSignalSpy errors(&processor, &DocumentProcessor::failed);
        processor.openDocument(source); QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, 10000);
        processor.renderDocumentPage(2); QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
        QCOMPARE(processor.renderedDpi(2), 300); QCOMPARE(QImage(ready[0][1].toString()).size(), QSize(1200,1600));
        processor.renderDocumentPage(2, 200); QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 2, 10000);
        QCOMPARE(QImage(ready[1][1].toString()).size(), QSize(800,1067));
        processor.renderDocumentPage(2); QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 3, 10000);
        QCOMPARE(processor.renderedDpi(2),300); QCOMPARE(ready[0][1].toString(), ready[2][1].toString());
        QVERIFY(errors.isEmpty());
    }
};
QTEST_MAIN(Release21Tests)
#include "release21_tests.moc"
