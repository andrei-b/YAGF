#include "documentintelligence.h"
#include "correctionmemory.h"
#include "formulatext.h"
#include "ocrreadings.h"
#include "pagegeometry.h"
#include "textraster.h"
#include "wordeditor.h"

#include <QGuiApplication>
#include <QFontDatabase>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace yagf::intelligence {
namespace {
using namespace core;

QString neighbor(const OcrPage& page, int index, int offset)
{
    const int other = index + offset;
    if (other < 0 || other >= page.words.size())
        return {};
    const auto& a = page.words[index];
    const auto& b = page.words[other];
    return a.block == b.block && a.paragraph == b.paragraph && a.line == b.line ? b.text : QString {};
}

QString numberInParentheses(const QString& text)
{
    static const QRegularExpression pattern(QStringLiteral(R"(^\(([0-9]+(?:\.[0-9]+)*)\)[.,;:]?$)"));
    return pattern.match(text.trimmed()).captured(1);
}

QString displayNumber(const QString& ending)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"((?:\\tag\{([0-9.]+)\}|\(([0-9]+(?:\.[0-9]+)*)\))\s*(?:\$\$?|\\\])?\s*$)"));
    const auto match = pattern.match(ending);
    return match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
}

bool isDisplay(const QString& type) { return type == "equation" || type == "formula" || type == "math"; }

QString referenceNumber(const OcrPage& page, int i)
{
    const auto& word = page.words[i];
    if (word.layoutBlock)
        return {};
    if (neighbor(page, i, -1) == "(" && neighbor(page, i, 1).startsWith(')'))
        return numberInParentheses("(" + word.text + ")");
    return numberInParentheses(word.text);
}

bool possibleContinuation(const OcrWord& last, const OcrWord& first)
{
    return !last.layoutBlock && !first.layoutBlock && last.text.size() > 1
        && last.text[last.text.size() - 2].isLetter() && last.text.endsWith('-') && !first.text.isEmpty()
        && first.text.front().isLower();
}

QString languageOf(const OcrDocument& document, const OcrPage& page)
{
    return page.language.isEmpty() ? document.language : page.language;
}

bool substantialOverlap(QRect a, QRect b)
{
    const auto area = [](QRect r) { return qint64(r.width()) * r.height(); };
    const auto overlap = a.intersected(b);
    return !overlap.isEmpty() && area(overlap) >= 0.65 * std::max(area(a), area(b));
}

bool coordinateList(const OcrPage& page, int i)
{
    static const QRegularExpression x(QStringLiteral("^[xх][,;]?$")), y(QStringLiteral("^[yу][,;]?$"));
    return x.match(neighbor(page, i, -2)).hasMatch() && y.match(neighbor(page, i, -1)).hasMatch();
}

bool inRunningText(const OcrPage& page, int index)
{
    const auto& word = page.words[index];
    for (const auto& area : page.imageRegions) {
        if ((area.notText || area.content == "math" || area.detectedLabel == "table")
            && area.shape().contains(word.bounds.center()))
            return false;
    }
    if (coordinateList(page, index))
        return true;
    for (int offset : { -3, -2, -1, 1, 2, 3 }) {
        const auto token = neighbor(page, index, offset);
        if (std::ranges::count_if(token, [](QChar c) { return c.isLetter(); }) >= 3)
            return true;
    }
    return false;
}

QString firstWord(const OcrRegion& r)
{
    if (r.paragraphs.isEmpty() || r.paragraphs.first().lines.isEmpty())
        return {};
    return r.paragraphs.first().lines.first().wordIds.value(0);
}

QString relationType(Relation r)
{
    if (r == Relation::RefersTo)
        return "reference";
    if (r == Relation::CaptionOf)
        return "caption";
    if (r == Relation::Continues)
        return "continuation";
    return {};
}

typeset::Ink softened(typeset::Ink input)
{
    if (!input.mask.isNull())
        input.mask = input.mask.scaled(64, 20, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                         .scaled(input.mask.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return input;
}

QString shortLabel(const QString& text) { return text.simplified().left(100); }
}

DocumentIntelligence::DocumentIntelligence(const OcrDocument& document, int pageNumber)
    : snapshot(document)
    , number(pageNumber)
{
    const auto* page = snapshot.page(number);
    if (!page)
        return;
    auto& nodes = graph.nodes;
    auto& edges = graph.edges;
    auto add = [&](Kind kind, QString id, QString label, QRect box, int pageNo, int word = -1) {
        nodes.append({ kind, std::move(id), std::move(label), pageNo, box, word, {} });
        return int(nodes.size() - 1);
    };
    auto link = [&](int a, int b, Relation relation) { edges.append({ a, b, relation }); };
    const int root = add(
        Kind::Page, page->id, QString("Page %1").arg(number), QRect(0, 0, page->width, page->height), number);
    QHash<QString, int> wordNodes, regionNodes;
    for (int i = 0; i < page->words.size(); ++i) {
        const auto& w = page->words[i];
        const int node = add(Kind::Word, w.id, shortLabel(w.text), w.bounds, number, i);
        nodes[node].anchor = w.id;
        graph.words.append(node);
        wordNodes.insert(w.id, node);
        if (i)
            link(graph.words[i - 1], node, Relation::Precedes);
    }
    QSet<int> displayWords;
    int previous = -1;
    for (const auto& region : page->regions) {
        const int r = add(Kind::Region, region.id, region.type, region.bounds, number);
        nodes[r].anchor = firstWord(region);
        regionNodes.insert(region.id, r);
        link(root, r, Relation::Contains);
        if (previous >= 0)
            link(previous, r, Relation::Precedes);
        previous = r;
        for (const auto& para : region.paragraphs) {
            const int p = add(Kind::Paragraph, para.id, "Paragraph", para.bounds, number);
            link(r, p, Relation::Contains);
            for (const auto& line : para.lines) {
                const int l = add(Kind::Line, line.id, "Line", line.bounds, number);
                link(p, l, Relation::Contains);
                for (const auto& id : line.wordIds) {
                    const int w = wordNodes.value(id, -1);
                    if (w < 0)
                        continue;
                    link(l, w, Relation::Contains);
                    if (isDisplay(region.type))
                        displayWords.insert(nodes[w].word);
                }
            }
        }
    }
    for (const auto& area : page->imageRegions) {
        const int a
            = add(Kind::Area, area.id, area.content.isEmpty() ? "Block" : area.content, area.bounds, number);
        link(root, a, Relation::Contains);
        const auto shape = area.shape();
        for (const auto& region : page->regions)
            if (shape.intersects(region.bounds))
                link(a, regionNodes.value(region.id), Relation::Overlaps);
        for (int i = 0; i < page->words.size(); ++i) {
            const auto& w = page->words[i];
            if (w.geometryInferred)
                continue;
            if (std::ranges::any_of(w.sourceBoxes, [&](QRect box) { return shape.intersects(box); }))
                link(a, graph.words[i], Relation::Overlaps);
        }
    }
    for (int i = 0; i < page->regions.size(); ++i) {
        const auto& caption = page->regions[i];
        if (caption.type != "caption")
            continue;
        // Only infer a link when exactly one adjacent figure aligns with this caption.
        int figure = -1;
        for (int j : { i - 1, i + 1 }) {
            if (j < 0 || j >= page->regions.size())
                continue;
            const auto& r = page->regions[j];
            if (!r.isFigure())
                continue;
            const auto c = caption.bounds;
            const int xOverlap
                = std::min(c.right(), r.bounds.right()) - std::max(c.left(), r.bounds.left()) + 1;
            const int gap = std::max(c.top() - r.bounds.bottom(), r.bounds.top() - c.bottom());
            if (xOverlap * 2 < std::min(c.width(), r.bounds.width()) || gap > std::max(c.height(), 8) * 2)
                continue;
            if (figure >= 0) {
                figure = -1;
                break;
            }
            figure = regionNodes.value(r.id);
        }
        if (figure >= 0)
            link(regionNodes.value(caption.id), figure, Relation::CaptionOf);
    }

    // A repeated display number is ambiguous (for example, in another chapter).
    struct Display {
        int page;
        QString id;
        QRect bounds;
        QString anchor;
    };
    QHash<QString, QVector<Display>> displays;
    for (const auto& p : snapshot.pages) {
        QHash<QString, int> indices;
        for (int i = 0; i < p.words.size(); ++i) {
            const auto& w = p.words[i];
            indices.insert(w.id, i);
            if (w.reviewed && (w.text == "z" || w.text == "z,") && coordinateList(p, i))
                coordinateExamples.append({ p.number, w.id, w.text, {}, {}, languageOf(snapshot, p) });
            if (isVerifiedCorrection(w))
                corrections.insert(w.originalText,
                    { p.number, w.id, w.text, neighbor(p, i, -1), neighbor(p, i, 1), languageOf(snapshot, p) });
        }
        for (const auto& region : p.regions) {
            if (!isDisplay(region.type) || region.paragraphs.isEmpty())
                continue;
            const auto& lines = region.paragraphs.last().lines;
            if (lines.isEmpty() || lines.last().wordIds.isEmpty())
                continue;
            const int i = indices.value(lines.last().wordIds.last(), -1);
            if (i < 0)
                continue;
            const auto label = displayNumber(formulaSourceText(p.words[i]));
            if (!label.isEmpty())
                displays[label].append({ p.number, region.id, region.bounds, firstWord(region) });
        }
    }
    QHash<QString, int> externalDisplays;
    for (int i = 0; i < page->words.size(); ++i) {
        const auto& word = page->words[i];
        if (i && word.block != page->words[i - 1].block && possibleContinuation(page->words[i - 1], word))
            link(graph.words[i - 1], graph.words[i], Relation::Continues);
        if (displayWords.contains(i))
            continue;
        const auto label = referenceNumber(*page, i);
        const auto found = displays.constFind(label);
        if (found == displays.cend() || found->size() != 1)
            continue;
        const auto& target = found->first();
        int r = regionNodes.value(target.id, -1);
        if (target.page != number) {
            r = externalDisplays.value(label, -1);
            if (r < 0) {
                r = add(
                    Kind::Region, target.id, QString("Equation (%1)").arg(label), target.bounds, target.page);
                nodes[r].anchor = target.anchor;
                externalDisplays.insert(label, r);
            }
        }
        if (r >= 0) {
            link(graph.words[i], r, Relation::RefersTo);
            references.insert(i);
        }
    }
    if (const auto* next = snapshot.page(number + 1); next && !next->words.isEmpty() && !page->words.isEmpty()
        && possibleContinuation(page->words.last(), next->words.first())) {
        const auto& w = next->words.first();
        const int target = add(Kind::Word, w.id, shortLabel(w.text), w.bounds, next->number);
        nodes[target].anchor = w.id;
        link(graph.words.last(), target, Relation::Continues);
    }
    // Restore editorial links even after a layout move makes the original geometric inference disappear.
    for (const auto& saved : page->reviewedRelations) {
        const auto rp = std::ranges::find(snapshot.pages, saved.targetPageId, &OcrPage::id);
        if (rp == snapshot.pages.end())
            continue;
        const auto kind = saved.type == "caption" ? Relation::CaptionOf
            : saved.type == "reference"           ? Relation::RefersTo
                                                  : Relation::Continues;
        const auto sourceKind = kind == Relation::CaptionOf ? Kind::Region : Kind::Word;
        const auto targetKind = kind == Relation::Continues ? Kind::Word : Kind::Region;
        const auto sameWord = [](const OcrPage& p, const QString& id, const QString& original) {
            const auto w = std::ranges::find(p.words, id, &OcrWord::id);
            return w != p.words.end() && w->originalText == original;
        };
        if (!sameWord(*page, saved.sourceWord, saved.sourceOriginal)
            || !sameWord(*rp, saved.targetWord, saved.targetOriginal))
            continue;
        auto findNode = [&](int pageNo, Kind k, const QString& anchor) {
            const auto it = std::ranges::find_if(
                nodes, [&](const Node& n) { return n.page == pageNo && n.kind == k && n.anchor == anchor; });
            return it == nodes.end() ? -1 : int(it - nodes.begin());
        };
        const int a = findNode(number, sourceKind, saved.sourceWord);
        int b = findNode(rp->number, targetKind, saved.targetWord);
        if (a < 0)
            continue;
        if (b < 0 && rp->number != number) {
            if (targetKind == Kind::Word) {
                const auto w = std::ranges::find(rp->words, saved.targetWord, &OcrWord::id);
                b = add(Kind::Word, w->id, shortLabel(w->text), w->bounds, rp->number);
            } else {
                const auto r = std::ranges::find_if(
                    rp->regions, [&](const auto& region) { return firstWord(region) == saved.targetWord; });
                if (r != rp->regions.end())
                    b = add(Kind::Region, r->id, r->type, r->bounds, rp->number);
            }
            if (b >= 0)
                nodes[b].anchor = saved.targetWord;
        }
        if (b < 0)
            continue;
        auto e = std::ranges::find_if(
            edges, [&](const Edge& item) { return item.from == a && item.to == b && item.relation == kind; });
        if (e == edges.end())
            edges.append({ a, b, kind, true });
        else
            e->confirmed = true;
        if (kind == Relation::RefersTo)
            references.insert(nodes[a].word);
    }
}

std::expected<OcrPage, QString> DocumentIntelligence::setConfirmed(int index, bool value) const
{
    if (index < 0 || index >= graph.edges.size())
        return std::unexpected(QStringLiteral("Unknown relation."));
    const auto& edge = graph.edges[index];
    const auto type = relationType(edge.relation);
    const auto &a = graph.nodes[edge.from], &b = graph.nodes[edge.to];
    const auto *p = snapshot.page(a.page), *other = snapshot.page(b.page);
    if (type.isEmpty() || !p || !other || p->number != number || other->id.isEmpty())
        return std::unexpected(QStringLiteral("This relation cannot be reviewed."));
    const auto from = std::ranges::find(p->words, a.anchor, &OcrWord::id);
    const auto to = std::ranges::find(other->words, b.anchor, &OcrWord::id);
    if (from == p->words.end() || to == other->words.end())
        return std::unexpected(QStringLiteral("Missing source word."));
    ReviewedRelation r { type, a.anchor, other->id, b.anchor, from->originalText, to->originalText };
    auto edited = *p;
    if (!value)
        edited.reviewedRelations.removeAll(r);
    else if (!edited.reviewedRelations.contains(r)) {
        if (edited.reviewedRelations.size() >= 10000 || r.sourceOriginal.size() > 65536
            || r.targetOriginal.size() > 65536)
            return std::unexpected(QStringLiteral("The relation exceeds the project limits."));
        edited.reviewedRelations.append(r);
    }
    return edited;
}

DocumentIntelligence::PageStyle DocumentIntelligence::styleFor(
    const OcrPage& page, const QImage& scan, int target) const
{
    if (styleImageKey == scan.cacheKey() && !styleWords.contains(target))
        return cachedStyle;
    PageStyle style { QStringLiteral("Liberation Serif"), false, 0 };
    struct Sample {
        QString text;
        typeset::Ink ink;
    };
    QVector<Sample> samples;
    QSet<QString> seen;
    styleWords.clear();
    for (int i = 0; i < page.words.size(); ++i) {
        if (i == target)
            continue;
        const auto& word = page.words[i];
        if (word.layoutBlock || word.italic || word.bold || word.geometryInferred || word.atOcrCropEdge
            || (!word.reviewed && word.confidence < 90) || word.text.size() < 5 || word.text.size() > 20
            || !std::ranges::all_of(word.text, [](QChar c) { return c.isLetter(); }) || seen.contains(word.text)
            || word.bounds.width() > 600 || word.bounds.height() > 100 || !scan.rect().contains(word.bounds))
            continue;
        auto ink = typeset::ink(scan.copy(word.bounds), false);
        if (ink.size.height() < 8)
            continue;
        seen.insert(word.text);
        samples.append({ word.text, std::move(ink) });
        styleWords.append(i);
        if (samples.size() == 5)
            break;
    }
    if (samples.size() >= 2) {
        double best = std::numeric_limits<double>::infinity();
        const auto installed = QFontDatabase::families();
        for (const auto& family :
            { "Liberation Serif", "DejaVu Serif", "Liberation Sans", "DejaVu Sans", "Liberation Mono" }) {
            if (!installed.contains(QLatin1String(family)))
                continue;
            QVector<double> sharp, soft;
            for (const auto& s : samples) {
                const auto rendered
                    = typeset::raster(s.text, QFont(QLatin1String(family)), s.ink.size, false).ink;
                sharp.append(typeset::distance(s.ink, rendered));
                soft.append(typeset::distance(s.ink, softened(rendered)));
            }
            std::ranges::sort(sharp);
            std::ranges::sort(soft);
            const int mid = samples.size() / 2;
            const auto loss = std::min(sharp[mid], soft[mid]);
            if (loss < best) {
                best = loss;
                style = { QLatin1String(family), soft[mid] < sharp[mid], int(samples.size()) };
            }
        }
    }
    cachedStyle = style;
    styleImageKey = scan.cacheKey();
    return style;
}

Assessment DocumentIntelligence::assess(int i, const QImage& scan) const
{
    Assessment result;
    const auto* p = snapshot.page(number);
    if (!p || i < 0 || i >= p->words.size())
        return result;
    const auto& w = p->words[i];
    auto add = [&](const QString& text, const QString& source) -> Hypothesis& {
        auto it = std::ranges::find(result.choices, text, &Hypothesis::text);
        if (it == result.choices.end()) {
            result.choices.append(Hypothesis {});
            it = result.choices.end() - 1;
            it->text = text;
        }
        if (!it->evidence.contains(source))
            it->evidence.append(source);
        return *it;
    };
    add(w.text, w.reviewed ? "Reviewed text" : "Current text");
    if (!w.readingEngine.isEmpty())
        add(w.text, w.readingEngine);
    if (!w.originalText.isEmpty())
        add(w.originalText, "Original OCR");
    if (!w.alternative.text.isEmpty() && p->specialistSpan(i).size() == 1)
        add(w.alternative.text, w.alternative.engine).runIds += w.alternative.historyIds;
    // Native region boxes belong to the provider's input frame, including after deskew or recropping.
    for (const auto& native : p->recognitionEvidence) {
        for (const auto& text : native.text) {
            if (!text.bounds || text.granularity != TextGranularity::Word || w.layoutBlock
                || w.geometryInferred)
                continue;
            QRect bounds;
            if (native.provenance.sourceToInput.isDefined()) {
                const auto outline = recognitionOutlineOnPage(native, text, p->coordinates);
                if (outline)
                    bounds = enclosingPixels(*outline);
            } else {
                const auto area = native.provenance.inputArea;
                if ((!area.isEmpty() && area.size() == native.provenance.imageSize)
                    || (area.isEmpty() && native.provenance.imageSize == QSize(p->width, p->height)))
                    bounds = text.bounds->translated(area.topLeft());
            }
            if (!text.text.isEmpty() && substantialOverlap(w.bounds, bounds))
                add(text.text, "OCR · " + native.engineId);
        }
    }
    for (const auto& reading : p->ocrReadings) {
        if (!reading.error.isEmpty())
            continue;
        for (const auto& item : reading.items) {
            QString label = reading.label;
            if (item.geometryInferred) {
                // A single transcription of a dedicated crop can be reviewed as crop-level evidence.
                // A paragraph-sized result must never be assigned to every word beneath it.
                if (!item.bounds.contains(w.bounds) || item.historyId.isEmpty()
                    || (w.layoutBlock != item.layoutBlock && item.text.contains(' ')))
                    continue;
                if (std::ranges::any_of(p->words, [&](const auto& other) {
                        return other.id != w.id && item.bounds.intersects(other.bounds);
                    }))
                    continue;
                label += " (submitted crop)";
            } else if (w.geometryInferred || !substantialOverlap(w.bounds, item.bounds)
                || (item.layoutBlock && !w.layoutBlock))
                continue;
            auto& h = add(item.text, label);
            if (!item.historyId.isEmpty() && !h.runIds.contains(item.historyId))
                h.runIds.append(item.historyId);
        }
    }
    const auto before = neighbor(*p, i, -1), after = neighbor(*p, i, 1);
    auto examples = corrections.equal_range(w.originalText);
    for (auto it = examples.first; it != examples.second; ++it) {
        const auto& ex = it.value();
        if ((ex.page == number && ex.id == w.id) || ex.language != languageOf(snapshot, *p))
            continue;
        auto& h = add(ex.text, QString("Verified on page %1 · %2").arg(ex.page).arg(ex.id));
        if ((!before.isEmpty() && ex.previous == before) || (!after.isEmpty() && ex.next == after))
            h.score = std::max(h.score, 0.08);
    }
    const bool protectedNumber = !referenceNumber(*p, i).isEmpty();
    if (references.contains(i))
        add(w.text, "Reference to a numbered equation").score += 0.25;
    // Preserve adjacent punctuation. An embedded digit or a multi-digit number is not a variable candidate.
    static const QRegularExpression two(QStringLiteral(R"(^2([,.;:]?)$)"));
    const auto digit = two.match(w.text);
    const bool formula = isFormulaWord(*p, w);
    if (!protectedNumber && !formula && !w.layoutBlock && digit.hasMatch() && inRunningText(*p, i)) {
        auto& z = add("z" + digit.captured(1), "Italic z / digit 2 ambiguity");
        if (coordinateList(*p, i)) {
            const auto fact = std::ranges::find_if(coordinateExamples, [&](const Example& ex) {
                return ex.language == languageOf(snapshot, *p) && (ex.page != number || ex.id != w.id);
            });
            if (fact != coordinateExamples.end()) {
                z.evidence.append(
                    QString("Verified coordinate list on page %1 · %2").arg(fact->page).arg(fact->id));
                z.score += 0.05;
            }
            z.evidence.append("Preceded by x and y on the same line");
            z.score += 0.055;
        }
    }
    if (formula) {
        for (auto& h : result.choices) {
            // Validate TeX grouping only for whole formula items. Escaped braces are printed symbols.
            if (!w.layoutBlock || !h.text.contains(QChar(92)))
                continue;
            int depth = 0;
            bool balanced = true;
            for (int j = 0; j < h.text.size(); ++j) {
                const auto c = h.text[j];
                if (c == QChar(92)) {
                    ++j;
                    continue;
                }
                if (c == '%') {
                    const auto end = h.text.indexOf('\n', j);
                    if (end < 0)
                        break;
                    j = end;
                    continue;
                }
                if (c == '{')
                    ++depth;
                if (c == '}' && --depth < 0) {
                    balanced = false;
                    break;
                }
            }
            if (!balanced || depth) {
                h.evidence.append("Unbalanced TeX group");
                h.score -= 0.2;
            }
        }
    }
    const bool aligned = scan.size() == QSize(p->width, p->height) && scan.rect().contains(w.bounds);
    const bool localized = !w.geometryInferred && !w.atOcrCropEdge && !w.layoutBlock
        && w.sourceBoxes.size() == 1 && w.sourceBoxes.first() == w.bounds;
    const bool renderable = std::ranges::all_of(result.choices, [](const auto& h) {
        return h.text.size() <= 12 && !h.text.isEmpty() && !h.text.contains(QRegularExpression("[\\s\\\\$]"));
    });
    if (result.choices.size() > 1 && aligned && localized && renderable && w.bounds.width() <= 512
        && w.bounds.height() <= 256 && qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        const auto source = typeset::ink(scan.copy(w.bounds), false);
        if (source.size.height() >= 6 && source.size.width() >= 3) {
            const auto style = styleFor(*p, scan, i);
            const QString family = w.fontFamily.isEmpty() ? style.family : w.fontFamily;
            QFont font(family);
            font.setBold(w.bold);
            for (auto& h : result.choices) {
                double best = std::numeric_limits<double>::infinity();
                for (bool italic : { false, true }) {
                    font.setItalic(italic);
                    auto rendered = typeset::raster(h.text, font, source.size, false).ink;
                    if (style.soften)
                        rendered = softened(rendered);
                    best = std::min(best, typeset::distance(source, rendered));
                }
                if (std::isfinite(best)) {
                    h.inkDistance = best;
                    h.score -= best;
                    h.evidence.append(w.fontFamily.isEmpty() && style.samples
                            ? QString("Rendered in %1, fitted on %2 prose words").arg(family).arg(style.samples)
                            : QString("Rendered in %1").arg(family));
                }
            }
        }
    }
    QHash<QString, QString> lastDecision;
    for (const auto& decision : p->recognitionHistory.decisions)
        if (decision.wordIds == QStringList { w.id })
            lastDecision.insert(decision.runId + QChar(0) + decision.proposedText, decision.decision);
    for (auto& h : result.choices) {
        if (h.text == w.text || h.runIds.isEmpty())
            continue;
        h.rejected = std::ranges::all_of(h.runIds,
            [&](const QString& id) { return lastDecision.value(id + QChar(0) + h.text) == "rejected"; });
        if (h.rejected) {
            h.evidence.append("Previously rejected for this word");
            h.score -= 1;
        }
    }
    std::stable_sort(result.choices.begin(), result.choices.end(),
        [](const auto& a, const auto& b) { return a.score > b.score; });
    if (w.reviewed || protectedNumber)
        result.suggested = w.text;
    else if (result.choices.size() > 1) {
        const auto& first = result.choices.first();
        const bool comparable
            = std::ranges::all_of(result.choices, [](const auto& h) { return h.inkDistance.has_value(); });
        if (comparable && !first.rejected && *first.inkDistance < 0.32
            && first.score - result.choices[1].score >= 0.04)
            result.suggested = first.text;
    }
    if (result.suggested.isEmpty() && !w.reviewed) {
        result.task = (formula || (digit.hasMatch() && coordinateList(*p, i))) ? "formula" : "text";
        const int pad = std::clamp(w.bounds.height() / 3, 2, 24);
        result.requestCrop
            = w.bounds.adjusted(-pad, -pad, pad, pad).intersected(QRect(0, 0, p->width, p->height));
    }
    return result;
}

QString DocumentIntelligence::preferredModel(const QStringList& keys, const QString& task) const
{
    struct Record {
        int accepted = 0, rejected = 0;
        qint64 ms = 0;
        int runs = 0;
    };
    QHash<QString, Record> stats;
    for (const auto& p : snapshot.pages) {
        QHash<QString, QString> sources;
        for (const auto& run : p.recognitionHistory.runs) {
            if (run.status != "succeeded")
                continue;
            const auto settings = run.preprocessing.value("recognition_options").toObject();
            const auto runTask = settings.value("task").toString();
            const bool formula = runTask == "formula" || settings.value("formula").toBool();
            if ((task == "formula") != formula || !keys.contains(run.source))
                continue;
            sources.insert(run.id, run.source);
            auto& s = stats[run.source];
            s.ms += run.elapsedMs;
            ++s.runs;
        }
        for (const auto& d : p.recognitionHistory.decisions) {
            if (!sources.contains(d.runId))
                continue;
            auto& s = stats[sources.value(d.runId)];
            if (d.decision == "accepted")
                ++s.accepted;
            if (d.decision == "rejected")
                ++s.rejected;
        }
    }
    // Smooth sparse review counts. At equal measured utility prefer the faster measured model.
    QString chosen;
    double best = -1, bestTime = std::numeric_limits<double>::infinity();
    for (const auto& key : keys) {
        const auto s = stats.value(key);
        const double utility = (s.accepted + 1.0) / (s.accepted + s.rejected + 2.0);
        const double time = s.runs ? double(s.ms) / s.runs : std::numeric_limits<double>::infinity();
        if (utility > best || (utility == best && time < bestTime)) {
            chosen = key;
            best = utility;
            bestTime = time;
        }
    }
    return chosen;
}

std::expected<OcrPage, QString> DocumentIntelligence::reject(int i, const Hypothesis& h) const
{
    const auto* p = snapshot.page(number);
    if (!p || i < 0 || i >= p->words.size() || h.text == p->words[i].text || h.runIds.isEmpty())
        return std::unexpected(QStringLiteral("Select an alternative with recorded OCR evidence."));
    auto edited = *p;
    for (const auto& id : h.runIds) {
        OcrReading evidence;
        evidence.historyIds = { id };
        recordReadingDecision(edited, *p, { i }, evidence, h.text, "rejected");
    }
    if (edited.recognitionHistory.decisions.size() == p->recognitionHistory.decisions.size())
        return std::unexpected(QStringLiteral("The source run could not be located on this page."));
    if (auto ok = validateRecognitionHistory(edited.recognitionHistory); !ok)
        return std::unexpected(ok.error());
    return edited;
}

std::expected<OcrPage, QString> DocumentIntelligence::accept(int i, const Hypothesis& h) const
{
    const auto* p = snapshot.page(number);
    if (!p || i < 0 || i >= p->words.size())
        return std::unexpected(QStringLiteral("Word no longer available."));
    const auto& w = p->words[i];
    if (!w.alternative.text.isEmpty() && h.text == w.alternative.text && p->specialistSpan(i).size() == 1)
        return WordEditor::acceptSpecialist(*p, i, h.text);
    OcrReading evidence;
    evidence.historyIds = h.runIds;
    QStringList labels;
    for (const auto& r : p->ocrReadings) {
        for (const auto& item : r.items) {
            if (item.text != h.text || !h.runIds.contains(item.historyId))
                continue;
            evidence.items.append(item);
            if (!labels.contains(r.label))
                labels.append(r.label);
        }
    }
    evidence.label = labels.join(", ");
    if (h.text == w.text) {
        auto edited = *p;
        edited.words[i].reviewed = true;
        if (!h.runIds.isEmpty())
            recordReadingDecision(edited, *p, { i }, evidence, h.text, "accepted");
        return edited;
    }
    if (!w.latexText.isEmpty())
        return std::unexpected(QStringLiteral("Use the formula editor for the canonical LaTeX reading."));
    return acceptOcrReading(*p, { i }, evidence, h.text);
}
}
