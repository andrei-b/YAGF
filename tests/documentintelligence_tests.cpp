#include "documentintelligence.h"
#include "intelligencedialog.h"
#include "documentcommands.h"
#include <QApplication>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QTreeWidget>
#include <QtTest>
#include <algorithm>

using namespace yagf;
using namespace intelligence;
namespace {
core::OcrWord word(QString text, QRect bounds, int block = 1)
{
    core::OcrWord w;
    w.text = w.originalText = text;
    w.bounds = bounds;
    w.confidence = 93;
    w.block = block;
    w.paragraph = w.line = 1;
    return w;
}
core::OcrPage page(int number, QVector<core::OcrWord> words)
{
    core::OcrPage p;
    p.number = number;
    p.width = 260;
    p.height = 120;
    p.language = "eng";
    p.words = words;
    p.id = QString("page-%1").arg(number);
    p.rebuildHierarchy();
    return p;
}
core::OcrDocument book()
{
    core::OcrDocument d;
    d.totalPages = 2;
    d.sourcePath = "book.pdf";
    d.language = "eng";
    auto p = page(1,
        { word("axes", { 5, 15, 25, 15 }), word("x,", { 40, 15, 15, 18 }), word("y,", { 65, 15, 15, 18 }),
            word("2", { 100, 10, 33, 40 }), word("(2)", { 190, 15, 26, 15 }),
            word("inter-", { 10, 80, 35, 18 }, 2) });
    core::ImageRegion area;
    area.id = "my-block";
    area.setShape(QRegion(5, 10, 25, 30).united(QRegion(95, 10, 50, 45)));
    p.imageRegions.append(area);
    auto second = page(2, { word("esting", { 10, 10, 30, 18 }), word("E=mc² (2)", { 40, 35, 100, 18 }, 2) });
    second.regions[1].type = "equation";
    d.pages = { p, second };
    return d;
}
QImage scan(const QString& text, bool italic)
{
    QImage image(260, 120, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter p(&image);
    QFont font("Liberation Serif");
    font.setPixelSize(36);
    font.setItalic(italic);
    p.setFont(font);
    p.drawText(QPoint(102, 42), text);
    return image;
}
}
class IntelligenceTests : public QObject {
    Q_OBJECT
private slots:
    void structureAndReview()
    {
        auto d = book();
        DocumentIntelligence engine(d, 1);
        const auto& g = engine.graph;
        const auto find
            = [&](Relation relation) { return std::ranges::find(g.edges, relation, &Edge::relation); };
        QVERIFY(find(Relation::Continues) != g.edges.end());
        const auto ref = find(Relation::RefersTo);
        QVERIFY(ref != g.edges.end());
        QCOMPARE(g.nodes[ref->to].page, 2);
        QCOMPARE(g.nodes[ref->from].word, 4);
        QVERIFY(std::ranges::none_of(g.edges, [&](const auto& e) {
            return e.relation == Relation::Overlaps && e.to == g.words[1]; // Hole in the drawn area.
        }));
        auto saved = engine.setConfirmed(int(ref - g.edges.begin()), true);
        QVERIFY(saved);
        d.replacePage(*saved);
        core::OcrDocument loaded;
        QString error;
        QVERIFY2(core::OcrDocument::fromJson(d.toJson(), &loaded, &error), qPrintable(error));
        loaded.pages[1].regions[1].type = "text"; // The confirmed relation survives loss of the heuristic.
        auto rebuilt = DocumentIntelligence(loaded, 1);
        QVERIFY(std::ranges::any_of(
            rebuilt.graph.edges, [](auto e) { return e.confirmed && e.relation == Relation::RefersTo; }));
        loaded.pages[1].words[1].originalText = "Unrelated OCR";
        auto unrelated = DocumentIntelligence(loaded, 1);
        QVERIFY(std::ranges::none_of(unrelated.graph.edges, [](auto e) { return e.confirmed; }));
        d.pages[0].regions[0].type = "figure";
        d.pages[0].regions[0].bounds = { 0, 0, 120, 72 };
        d.pages[0].regions[1].type = "caption";
        auto caption = DocumentIntelligence(d, 1);
        QVERIFY(
            std::ranges::any_of(caption.graph.edges, [](auto e) { return e.relation == Relation::CaptionOf; }));
    }
    void inkAndAbstention()
    {
        auto d = book();
        DocumentIntelligence engine(d, 1);
        const auto z = engine.assess(3, scan("z", true));
        QCOMPARE(z.suggested, "z");
        const auto two = engine.assess(3, scan("2", false));
        QCOMPARE(two.suggested, "2");
        QCOMPARE(engine.assess(4, {}).suggested, "(2)");
        QVERIFY(engine.assess(3, QImage(100, 100, QImage::Format_RGB32)).suggested.isEmpty());
        d.pages[0].words[3].atOcrCropEdge = true;
        const auto clipped = DocumentIntelligence(d, 1).assess(3, scan("z", true));
        QVERIFY(clipped.suggested.isEmpty());
        QVERIFY(clipped.requestCrop.contains(d.pages[0].words[3].bounds));
        d.pages[0].words[3].text = "22";
        QCOMPARE(DocumentIntelligence(d, 1).assess(3, {}).choices.size(), 2); // Current and original only.
    }
    void memoryProvenanceAndUndo()
    {
        auto d = book();
        auto& p = d.pages[0];
        auto& w = p.words[3];
        core::OcrReading second;
        second.label = "Other OCR";
        second.engine = "mock";
        second.source = "mock";
        second.items.append({ w.bounds, "z", 1, 1, 1, false, false, "source-run" });
        second.historyIds = { "source-run" };
        p.ocrReadings.append(second);
        core::RecognitionRun run;
        run.id = "source-run";
        run.engine = "mock";
        run.source = "mock";
        run.status = "succeeded";
        run.startedUtc = "2025-01-01T00:00:00.000Z";
        run.inputSize = w.bounds.size();
        run.processedCrop = w.bounds;
        p.recognitionHistory.runs.append(run);
        auto memoryPage = page(2, p.words);
        memoryPage.words[3].id = "verified-z";
        memoryPage.words[3].text = "z";
        memoryPage.words[3].reviewed = true;
        d.pages[1] = memoryPage;
        DocumentIntelligence engine(d, 1);
        const auto assessment = engine.assess(3, scan("z", true));
        const auto& candidate = assessment.choices.first();
        QVERIFY(candidate.evidence.join(' ').contains("Verified coordinate list"));
        QVERIFY(candidate.runIds.contains("source-run"));
        auto rejected = engine.reject(3, candidate);
        QVERIFY(rejected);
        QCOMPARE(rejected->recognitionHistory.decisions.last().decision, "rejected");
        auto copy = d;
        copy.replacePage(*rejected);
        const auto afterRejection = DocumentIntelligence(copy, 1).assess(3, scan("z", true));
        QVERIFY(afterRejection.suggested != "z");
        QUndoStack stack;
        const auto accepted = engine.accept(3, candidate);
        QVERIFY(accepted);
        stack.push(new ui::ReplacePageCommand(d, 1, *accepted, "review"));
        QCOMPARE(d.page(1)->words[3].text, "z");
        QVERIFY(d.page(1)->words[3].reviewed);
        stack.undo();
        QCOMPARE(d.page(1)->words[3].text, "2");
        QVERIFY(!d.page(1)->words[3].reviewed);
        d.pages[1].words[3].reviewed = false;
        const auto after = DocumentIntelligence(d, 1).assess(3, {});
        for (const auto& h : after.choices)
            QVERIFY(!h.evidence.join(' ').contains("Verified"));
    }
    void nativeGeometryAndFormulaValidation()
    {
        auto d = book();
        auto& p = d.pages.first();
        core::RecognitionResult native;
        native.engineId = "new-provider";
        native.provenance.imageSize = { 45, 62 };
        native.provenance.inputArea = { 94, 4, 45, 62 };
        core::TextRegion item;
        item.text = "s";
        item.bounds = QRect(6, 6, 33, 40);
        item.granularity = core::TextGranularity::Word;
        native.text.append(item);
        item.text = "entire line";
        item.granularity = core::TextGranularity::Line;
        native.text.append(item);
        p.recognitionEvidence = { native };
        const auto a = DocumentIntelligence(d, 1).assess(3, {});
        QVERIFY(std::ranges::find(a.choices, QString("s"), &Hypothesis::text) != a.choices.end());
        QVERIFY(std::ranges::find(a.choices, QString("entire line"), &Hypothesis::text) == a.choices.end());
        p.recognitionEvidence.first().provenance.inputArea.setWidth(20);
        const auto incompatible = DocumentIntelligence(d, 1).assess(3, {});
        QVERIFY(std::ranges::find(incompatible.choices, QString("s"), &Hypothesis::text)
            == incompatible.choices.end());

        auto& display = d.pages[1].words[1];
        display.layoutBlock = true;
        display.text = QStringLiteral(R"(\frac{1}{2)");
        auto formula = DocumentIntelligence(d, 2).assess(1, {});
        const auto bad = std::ranges::find(formula.choices, display.text, &Hypothesis::text);
        QVERIFY(bad != formula.choices.end() && bad->evidence.contains("Unbalanced TeX group"));
        QCOMPARE(formula.task, "formula");
        display.text = QStringLiteral(R"(\{x\} + \frac{1}{2})");
        formula = DocumentIntelligence(d, 2).assess(1, {});
        for (const auto& h : formula.choices)
            QVERIFY(!h.evidence.contains("Unbalanced TeX group"));
    }

    void modelSelectionAndDialog()
    {
        auto d = book();
        core::RecognitionRun run;
        run.source = "slow";
        run.id = "r";
        run.elapsedMs = 200;
        run.status = "succeeded";
        d.pages[1].recognitionHistory.runs = { run };
        core::RecognitionDecision decision;
        decision.runId = "r";
        decision.decision = "rejected";
        d.pages[1].recognitionHistory.decisions = { decision };
        DocumentIntelligence engine(d, 1);
        QCOMPARE(engine.preferredModel({ "slow", "other" }, "text"), "other");
        QCOMPARE(engine.preferredModel({ "slow", "other" }, "formula"), "slow");
        ui::IntelligenceDialog dialog(d, 1, scan("z", true), {}, {}, 3, nullptr);
        auto* choices = dialog.findChild<QListWidget*>("intelligenceReadings");
        QVERIFY(choices && choices->count() == 2 && choices->currentItem()->text().startsWith("z "));
        dialog.findChild<QPushButton*>("useIntelligenceReading")->click();
        QVERIFY(dialog.edited);
        QCOMPARE(dialog.edited->words[3].text, "z");
        ui::IntelligenceDialog block(d, 1, scan("z", true), {}, "my-block", -1, nullptr);
        QCOMPARE(block.findChild<QTreeWidget*>()->currentItem()->toolTip(0), "Page 1 · my-block");
    }
};
QTEST_MAIN(IntelligenceTests)
#include "documentintelligence_tests.moc"
