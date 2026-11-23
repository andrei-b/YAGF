#include "recognitionadapter.h"
#include "formulatext.h"
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
    // Formula children can supplement a prose-only parent.
    for (const auto &r : result.tables) if (!r.parentId.isEmpty()) parents.insert(r.parentId);
    for (const auto &r : result.figures) if (!r.parentId.isEmpty()) parents.insert(r.parentId);
    QHash<QString, int> parentBlocks, parentParagraphs, parentLines;
    const auto addText = [&](const TextRegion &r) {
        int block = r.block, paragraph = r.paragraph, line = r.line;
        QString ancestor = r.parentId, root = r.id, paragraphId, lineId, type = r.type;
        if (r.granularity == TextGranularity::Paragraph) paragraphId = r.id;
        if (r.granularity == TextGranularity::Line) lineId = r.id;
        while (nodes.contains(ancestor)) {
            const auto *parent = nodes[ancestor]; root = ancestor;
            if (!block) block = parent->block;
            if (!paragraph) paragraph = parent->paragraph;
            if (!line) line = parent->line;
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
    };
    for (const auto &r : result.text) {
        if (!r.id.isEmpty() && parents.contains(r.id)) continue;
        addText(r);
    }
    for (const auto &r : result.formulas) {
        if (r.display) {
            add(r, r.latex, true, "equation");
        } else {
            const auto *parent = nodes.value(r.parentId);
            if (parent && !parents.contains(parent->id) &&
                std::ranges::any_of(mathTextSpans(parent->text), [&](const auto &span) {
                    return !span.display && span.source == r.latex;
                })) continue;
            TextRegion inlineWord;
            static_cast<RecognitionRegion &>(inlineWord) = r;
            inlineWord.text = "\\(" + r.latex + "\\)";
            inlineWord.granularity = TextGranularity::Word;
            addText(inlineWord);
        }
    }
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
    QHash<QString, QString> nativeParents;
    QHash<QString, TextRegion> nativeText;
    for (const auto &reading : page.recognitionEvidence) {
        for (const auto &text : reading.text) {
            nativeParents.insert(text.id, text.parentId);
            nativeText.insert(text.id, text);
        }
        for (const auto &formula : reading.formulas) nativeParents.insert(formula.id, formula.parentId);
    }
    QSet<QString> emittedIds;
    for (int i = 0; i < page.words.size(); ++i) {
        const auto &word = page.words[i]; const auto region = regions.value(word.id);
        RecognitionRegion base;
        base.id = word.id;
        base.parentId = nativeParents.value(word.id);
        emittedIds.insert(word.id);
        if (word.geometryInferred) base.inputArea = word.bounds;
        else base.bounds = word.bounds;
        base.order = i; base.confidence = word.confidence;
        const auto source = formulaSourceText(word);
        const auto spans = mathTextSpans(source);
        const bool delimited = spans.size() == 1 && spans.first().length == source.size();
        if (region.type != "table" && (delimited || region.type == "equation" || region.type == "formula")) {
            FormulaRegion formula; static_cast<RecognitionRegion &>(formula) = base;
            formula.latex = delimited ? spans.first().source : source;
            formula.display = !delimited || spans.first().display;
            result.formulas.append(formula);
        } else if (region.type == "table") {
            TableRegion table; static_cast<RecognitionRegion &>(table) = base; table.text = source;
            if (!region.tableLayout.isEmpty()) table.structure["table_layout"] = region.tableLayout;
            result.tables.append(table);
        } else {
            TextRegion text; static_cast<RecognitionRegion &>(text) = base; text.text = source;
            text.block = word.block; text.paragraph = word.paragraph; text.line = word.line; text.type = region.type;
            text.granularity = word.layoutBlock ? TextGranularity::Block : TextGranularity::Word;
            result.text.append(text);
            int occurrence = 0;
            for (const auto &span : spans) {
                if (span.display) continue;
                FormulaRegion child;
                child.id = word.id + QStringLiteral("/inline-%1").arg(++occurrence);
                child.parentId = word.id;
                child.order = i;
                child.latex = span.source;
                child.display = false;
                child.inputArea = word.bounds;
                // Several formulas can share one coarse OCR box. Only attach
                // precise geometry if there is evidence for this occurrence.
                for (auto decision=page.recognitionHistory.decisions.crbegin();
                     decision!=page.recognitionHistory.decisions.crend(); ++decision) {
                    if (decision->wordIds.contains(word.id) && decision->proposedText == span.source &&
                        decision->decision == "accepted" && word.bounds.intersects(decision->processedArea) &&
                        std::ranges::count_if(spans,[&](const auto &s){return s.source==span.source;}) == 1) {
                        child.inputArea = decision->processedArea;
                        break;
                    }
                }
                result.formulas.append(child);
            }
        }
    }
    // Recreate structural ancestors omitted from the editable leaf sequence.
    // Their coordinates may belong to a crop, so only use the current page frame.
    const auto restoreParents = [&](RecognitionRegion &child) {
        QString ancestor = child.parentId;
        if (!ancestor.isEmpty() && !emittedIds.contains(ancestor) && !nativeText.contains(ancestor)) {
            child.parentId.clear(); return;
        }
        while (!ancestor.isEmpty() && !emittedIds.contains(ancestor)) {
            auto parent = nativeText.value(ancestor);
            if (parent.id.isEmpty()) break;
            emittedIds.insert(ancestor);
            parent.text.clear();
            parent.bounds.reset();
            parent.inputArea = QRect(QPoint(), result.provenance.imageSize);
            result.text.append(parent);
            ancestor = parent.parentId;
        }
    };
    for (auto &formula : result.formulas) restoreParents(formula);
    for (int i=0; i<result.text.size(); ++i) {
        auto text = result.text[i];
        restoreParents(text);
        result.text[i].parentId = text.parentId;
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
