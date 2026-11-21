#include "recognitionadapter.h"
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace yagf::processing {
using namespace core;

std::expected<OcrPage, QString> recognitionToPage(const RecognitionResult &result) {
    if (auto valid = validateRecognitionResult(result); !valid) return std::unexpected(valid.error());
    OcrPage page;
    const auto &p = result.provenance;
    page.id = QStringLiteral("page-%1").arg(p.pageNumber); page.number = p.pageNumber;
    page.width = page.sourceWidth = p.imageSize.width(); page.height = page.sourceHeight = p.imageSize.height();
    page.language = p.language; page.engineId = result.engineId; page.recognitionEvidence.append(result);
    struct Item { OcrWord word; int order; QString type; QJsonArray tableLayout; };
    QVector<Item> items;
    int nextBlock = 0;
    for (const auto &r : result.text) nextBlock = std::max(nextBlock, r.block);
    const auto add = [&](const RecognitionRegion &r, QString text, bool layout, QString type,
                         int block = 0, int paragraph = 1, int line = 1, QJsonArray tableLayout = {}) {
        if (text.isEmpty()) return;
        OcrWord word; word.page = page.number;
        word.block = block > 0 ? block : ++nextBlock; word.paragraph = std::max(1, paragraph); word.line = std::max(1, line);
        word.bounds = r.bounds.value_or(r.inputArea.value_or(QRect(QPoint(), p.imageSize))); word.geometryInferred = !r.bounds;
        word.layoutBlock = layout || word.geometryInferred; word.text = word.originalText = text;
        word.confidence = r.confidence; word.id = r.id; word.readingEngine = result.engineId;
        items.append({std::move(word), r.order, std::move(type), std::move(tableLayout)});
    };
    QSet<QString> parents;
    QHash<QString, const TextRegion *> nodes;
    for (const auto &r : result.text) if (!r.id.isEmpty()) nodes.insert(r.id, &r);
    for (const auto &r : result.text) if (!r.parentId.isEmpty()) parents.insert(r.parentId);
    for (const auto &r : result.formulas) if (!r.parentId.isEmpty()) parents.insert(r.parentId);
    for (const auto &r : result.tables) if (!r.parentId.isEmpty()) parents.insert(r.parentId);
    for (const auto &r : result.figures) if (!r.parentId.isEmpty()) parents.insert(r.parentId);
    QHash<QString, int> parentBlocks, parentParagraphs, parentLines;
    for (const auto &r : result.text) {
        if (!r.id.isEmpty() && parents.contains(r.id)) continue; // Prefer detailed children over duplicated parent text.
        int block = r.block, paragraph = r.paragraph, line = r.line;
        QString ancestor = r.parentId, root, paragraphId, lineId, type = r.type;
        while (nodes.contains(ancestor)) {
            const auto *parent = nodes[ancestor]; root = ancestor;
            if (parent->granularity == TextGranularity::Paragraph) paragraphId = ancestor;
            if (parent->granularity == TextGranularity::Line) lineId = ancestor;
            if (type == "text" && parent->type != "text") type = parent->type;
            ancestor = parent->parentId;
        }
        if (!root.isEmpty() && block <= 0) {
            if (!parentBlocks.contains(root)) parentBlocks.insert(root, ++nextBlock);
            block = parentBlocks[root];
        }
        if (!paragraphId.isEmpty() && paragraph <= 0) {
            if (!parentParagraphs.contains(paragraphId)) parentParagraphs.insert(paragraphId, parentParagraphs.size()+1);
            paragraph = parentParagraphs[paragraphId];
        }
        if (!lineId.isEmpty() && line <= 0) {
            if (!parentLines.contains(lineId)) parentLines.insert(lineId, parentLines.size()+1);
            line = parentLines[lineId];
        }
        add(r, r.text, r.granularity != TextGranularity::Word, type, block, paragraph, line);
    }
    for (const auto &r : result.formulas) add(r, r.display ? r.latex : "\\("+r.latex+"\\)", true, "equation");
    for (const auto &r : result.tables) {
        const auto layout = r.structure.value("table_layout").toArray();
        if (!layout.isEmpty() && !OcrRegion::validTableLayout(layout))
            return std::unexpected(QStringLiteral("Invalid recognition table layout."));
        add(r, !r.text.isEmpty() ? r.text : !r.markdown.isEmpty() ? r.markdown : r.html, true, "table", 0, 1, 1, layout);
    }
    for (const auto &r : result.figures) {
        const auto id = r.id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : r.id;
        if (r.bounds) page.imageRegions.append({id, *r.bounds, p.language, r.type != "unclassified",
            r.type == "unclassified" ? QString() : QStringLiteral("picture"), true});
        if (!r.caption.isEmpty()) add(r, r.caption, true, "caption");
    }
    if (items.isEmpty() && !result.markdown.isEmpty()) {
        RecognitionRegion unlocated;
        add(unlocated, result.markdown, true, "text");
    }
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        // Unspecified order retains the vector's native sequence.
        return (a.order < 0 ? 1000001 : a.order) < (b.order < 0 ? 1000001 : b.order);
    });
    for (const auto &item : items) page.words.append(item.word);
    page.rebuildHierarchy();
    int regionIndex = -1, previousBlock = -1;
    for (int i = 0; i < page.words.size(); ++i) {
        const int block = page.words[i].block;
        if (block != previousBlock) { ++regionIndex; previousBlock = block; }
        auto &region = page.regions[regionIndex];
        region.type = items[i].type; region.tableLayout = items[i].tableLayout;
    }
    return page;
}

RecognitionResult recognitionFromPage(const OcrPage &page) {
    RecognitionResult result; result.engineId = page.engineId;
    result.provenance.language = page.language; result.provenance.pageNumber = page.number;
    result.provenance.imageSize = {page.width, page.height};
    QHash<QString, OcrRegion> regions;
    for (const auto &region : page.regions) for (const auto &paragraph : region.paragraphs)
        for (const auto &line : paragraph.lines) for (const auto &id : line.wordIds) regions.insert(id, region);
    for (int i = 0; i < page.words.size(); ++i) {
        const auto &word = page.words[i]; const auto region = regions.value(word.id);
        RecognitionRegion base;
        base.id = word.id;
        if (word.geometryInferred) base.inputArea = word.bounds;
        else base.bounds = word.bounds;
        base.order = i; base.confidence = word.confidence;
        if (region.type == "equation" || region.type == "formula") {
            FormulaRegion formula; static_cast<RecognitionRegion &>(formula) = base; formula.latex = word.text;
            result.formulas.append(formula);
        } else if (region.type == "table") {
            TableRegion table; static_cast<RecognitionRegion &>(table) = base; table.text = word.text;
            if (!region.tableLayout.isEmpty()) table.structure["table_layout"] = region.tableLayout;
            result.tables.append(table);
        } else {
            TextRegion text; static_cast<RecognitionRegion &>(text) = base; text.text = word.text;
            text.block = word.block; text.paragraph = word.paragraph; text.line = word.line; text.type = region.type;
            text.granularity = word.layoutBlock ? TextGranularity::Block : TextGranularity::Word;
            result.text.append(text);
        }
    }
    for (const auto &hint : page.imageRegions) {
        FigureRegion figure; figure.id = hint.id; figure.bounds = hint.bounds;
        figure.type = hint.content.isEmpty() ? "unclassified" : "figure"; result.figures.append(figure);
    }
    return result;
}

void appendRecognitionEvidence(OcrPage &page, const OcrPage &reading, QPoint offset) {
    core::mergeRecognitionHistory(page.recognitionHistory,reading.recognitionHistory);
    const auto incoming = reading.recognitionEvidence;
    for (auto result : incoming) {
        auto &area = result.provenance.inputArea;
        if (area.isEmpty()) area = QRect(QPoint(), result.provenance.imageSize);
        area.translate(offset);
        page.recognitionEvidence.removeIf([&](const auto &old) {
            const auto oldArea = old.provenance.inputArea.isEmpty() ? QRect(QPoint(), old.provenance.imageSize) : old.provenance.inputArea;
            return old.engineId == result.engineId && old.provenance.model == result.provenance.model &&
                old.provenance.task == result.provenance.task && oldArea == area;
        });
        page.recognitionEvidence.append(std::move(result));
    }
}
}
