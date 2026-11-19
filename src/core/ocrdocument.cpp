#include "ocrdocument.h"

#include <QJsonArray>
#include <QHash>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace yagf::core {

namespace {
bool integer(const QJsonObject &object, const QString &key, int *value)
{
    const QJsonValue field = object.value(key);
    if (!field.isDouble()) return false;
    const double n = field.toDouble();
    if (!std::isfinite(n) || n != std::floor(n) ||
        n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max()) return false;
    *value = static_cast<int>(n);
    return true;
}

QString pageId(int page) { return QStringLiteral("page-%1").arg(page); }
QString regionId(int page, int block) { return QStringLiteral("%1/region-%2").arg(pageId(page)).arg(block); }
QString paragraphId(int page, int block, int paragraph)
{ return QStringLiteral("%1/paragraph-%2").arg(regionId(page, block)).arg(paragraph); }
QString lineId(int page, int block, int paragraph, int line)
{ return QStringLiteral("%1/line-%2").arg(paragraphId(page, block, paragraph)).arg(line); }

QJsonObject boxJson(const QRect &r)
{
    return {{"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()}};
}

bool parseBox(const QJsonObject &json, int pageWidth, int pageHeight, QRect *box)
{
    int x = 0, y = 0, w = 0, h = 0;
    if (!integer(json, QStringLiteral("x"), &x) || !integer(json, QStringLiteral("y"), &y) ||
        !integer(json, QStringLiteral("width"), &w) || !integer(json, QStringLiteral("height"), &h) ||
        x < 0 || y < 0 || w <= 0 || h <= 0 ||
        static_cast<qint64>(x) + w > pageWidth || static_cast<qint64>(y) + h > pageHeight) return false;
    *box = QRect(x, y, w, h);
    return true;
}

QJsonObject wordJson(const OcrWord &word)
{
    QJsonArray sources;
    for (const QString &id : word.sourceWordIds) sources.append(id);
    QJsonArray sourceBoxes;
    for (const QRect &box : word.sourceBoxes) sourceBoxes.append(boxJson(box));
    QJsonObject result{{"id", word.id}, {"page", word.page}, {"block", word.block},
            {"paragraph", word.paragraph}, {"line", word.line}, {"box", boxJson(word.bounds)},
            {"original_text", word.originalText}, {"text", word.text},
            {"source_word_ids", sources},
            {"source_boxes", sourceBoxes},
            {"confidence", word.confidence}, {"reviewed", word.reviewed},
            {"style", QJsonObject{{"bold", word.bold}, {"italic", word.italic},
                                   {"font_family", word.fontFamily},
                                   {"source_font_size_px", word.sourceFontSizePx},
                                   {"font_size_pt", word.fontSizePt},
                                   {"source_font_size_pt", word.sourceFontSizePt}}}};
    if (!word.readingEngine.isEmpty()) result.insert("reading_engine", word.readingEngine);
    if (!word.alternative.sourceTokens.isEmpty()) {
        const auto &a = word.alternative;
        result.insert("specialist_reading", QJsonObject{{"engine", a.engine}, {"text", a.text},
            {"source_tokens", QJsonArray::fromStringList(a.sourceTokens)}, {"formula", a.formula}});
    }
    if (!word.symbolShape.isEmpty()) result.insert("symbol_shape", QString::fromLatin1(word.symbolShape.toHex()));
    if (word.layoutBlock) result.insert("layout_block", true);
    return result;
}

bool parseWord(const QJsonObject &json, int pageNumber, int width, int height, OcrWord *word)
{
    if (!integer(json, QStringLiteral("page"), &word->page) || word->page != pageNumber ||
        !integer(json, QStringLiteral("block"), &word->block) ||
        !integer(json, QStringLiteral("paragraph"), &word->paragraph) ||
        !integer(json, QStringLiteral("line"), &word->line) ||
        !parseBox(json.value(QStringLiteral("box")).toObject(), width, height, &word->bounds) ||
        !json.value(QStringLiteral("original_text")).isString() ||
        !json.value(QStringLiteral("text")).isString() ||
        !json.value(QStringLiteral("confidence")).isDouble() ||
        !json.value(QStringLiteral("reviewed")).isBool()) return false;
    word->readingEngine = json.value("reading_engine").toString();
    if (json.contains("specialist_reading")) {
        const auto obj = json.value("specialist_reading").toObject();
        auto &a = word->alternative;
        a.engine = obj.value("engine").toString();
        a.text = obj.value("text").toString();
        a.formula = obj.value("formula").toBool();
        const auto tokens = obj.value("source_tokens").toArray();
        if (tokens.isEmpty() || tokens.size() > 1024 || a.text.trimmed().isEmpty() ||
            a.text.size() > 16384 || a.engine.isEmpty() || !obj.value("formula").isBool()) return false;
        for (const auto &token : tokens) {
            if (!token.isString() || token.toString().isEmpty()) return false;
            a.sourceTokens.append(token.toString());
        }
    }
    if (json.contains("layout_block") && !json.value("layout_block").isBool()) return false;
    word->layoutBlock = json.value("layout_block").toBool();
    if (json.contains("symbol_shape")) {
        const QByteArray hex = json.value("symbol_shape").toString().toLatin1();
        if (hex.size() != 64) return false;
        word->symbolShape = QByteArray::fromHex(hex);
        if (word->symbolShape.toHex() != hex.toLower()) return false;
    }
    word->originalText = json.value(QStringLiteral("original_text")).toString();
    word->text = json.value(QStringLiteral("text")).toString();
    word->confidence = json.value(QStringLiteral("confidence")).toDouble();
    word->reviewed = json.value(QStringLiteral("reviewed")).toBool();
    word->id = json.value(QStringLiteral("id")).toString();
    if (json.contains(QStringLiteral("source_word_ids"))) {
        const QJsonValue sources = json.value(QStringLiteral("source_word_ids"));
        if (!sources.isArray()) return false;
        for (const auto &value : sources.toArray()) {
            if (!value.isString() || value.toString().isEmpty()) return false;
            word->sourceWordIds.append(value.toString());
        }
    } else if (!word->id.isEmpty()) word->sourceWordIds.append(word->id);
    if (json.contains(QStringLiteral("source_boxes"))) {
        const QJsonValue boxes = json.value(QStringLiteral("source_boxes"));
        if (!boxes.isArray() || boxes.toArray().size() > 4096) return false;
        for (const auto &value : boxes.toArray()) {
            QRect box;
            if (!value.isObject() || !parseBox(value.toObject(), width, height, &box) ||
                !word->bounds.contains(box)) return false;
            word->sourceBoxes.append(box);
        }
    } else if (!word->originalText.isEmpty()) word->sourceBoxes.append(word->bounds);
    const QJsonObject style = json.value(QStringLiteral("style")).toObject();
    word->bold = style.value(QStringLiteral("bold")).toBool();
    word->italic = style.value(QStringLiteral("italic")).toBool();
    if (style.contains("source_font_size_px") &&
        (!integer(style, "source_font_size_px", &word->sourceFontSizePx) ||
         word->sourceFontSizePx < 0 || word->sourceFontSizePx > 4096)) return false;
    if (style.contains("font_family")) {
        const auto family = style.value("font_family");
        if (!family.isString() || family.toString().size() > 128) return false;
        word->fontFamily = family.toString();
        for (QChar c : word->fontFamily) if (c.category() == QChar::Other_Control) return false;
    }
    if (style.contains(QStringLiteral("font_size_pt")) &&
        (!integer(style, QStringLiteral("font_size_pt"), &word->fontSizePt) ||
         word->fontSizePt < 0 || word->fontSizePt > 72)) return false;
    if (style.contains(QStringLiteral("source_font_size_pt")) &&
        (!integer(style, QStringLiteral("source_font_size_pt"), &word->sourceFontSizePt) ||
         word->sourceFontSizePt < 0 || word->sourceFontSizePt > 72)) return false;
    return std::isfinite(word->confidence);
}
}

QString OcrWord::fontFamilyCss() const
{
    if (fontFamily.isEmpty()) return {};
    QString escaped = fontFamily;
    escaped.replace('\\', QStringLiteral("\\\\"));
    escaped.replace('\'', QStringLiteral("\\'"));
    escaped.replace('\n', ' ');
    escaped.replace('\r', ' ');
    return QStringLiteral("font-family:'%1';").arg(escaped);
}

bool OcrRegion::isFigure() const
{
    return type == "figure" || type == "diagram" || type == "picture" || type == "image";
}

bool OcrRegion::validTableLayout(const QJsonValue &value)
{
    if (!value.isArray()) return false;
    qsizetype cells = 0;
    for (const auto &row : value.toArray()) {
        if (!row.isArray() || row.toArray().isEmpty()) return false;
        cells += row.toArray().size();
        if (cells > 100000) return false;
        for (const auto &entry : row.toArray()) {
            const auto cell = entry.toObject();
            int rows, columns;
            if (!cell.value("header").isBool() || !integer(cell, "rowspan", &rows) ||
                !integer(cell, "colspan", &columns) || rows < 1 || columns < 1 ||
                rows > 999 || columns > 999) return false;
        }
    }
    return true;
}

QVector<int> OcrPage::specialistSpan(int start) const
{
    if (start < 0 || start >= words.size()) return {};
    const auto &first = words[start];
    const auto &tokens = first.alternative.sourceTokens;
    if (tokens.size() > words.size() - start) return {};
    QVector<int> indices;
    for (int i = 0; i < tokens.size(); ++i) {
        const auto &word = words[start + i];
        if (word.reviewed || word.originalText != tokens[i] || word.text != tokens[i] ||
            word.block != first.block || word.paragraph != first.paragraph || word.line != first.line)
            return {};
        indices.append(start + i);
    }
    return indices;
}

void OcrPage::rebuildHierarchy()
{
    QHash<QString, OcrRegion> previousRegions;
    for (const OcrRegion &region : regions) previousRegions.insert(region.id, region);
    regions.clear();
    int previousBlock = -1, previousParagraph = -1, previousLine = -1;
    for (int i = 0; i < words.size(); ++i) {
        OcrWord &word = words[i];
        word.page = number;
        if (word.id.isEmpty())
            word.id = QStringLiteral("%1/word-%2").arg(lineId(number, word.block, word.paragraph, word.line)).arg(i + 1);
        if (word.sourceWordIds.isEmpty() && !word.originalText.isEmpty())
            word.sourceWordIds.append(word.id);
        if (word.sourceBoxes.isEmpty() && !word.originalText.isEmpty())
            word.sourceBoxes.append(word.bounds);
        if (regions.isEmpty() || word.block != previousBlock) {
            OcrRegion region;
            region.id = regionId(number, word.block);
            regions.append(region);
            previousParagraph = -1;
            previousLine = -1;
        }
        OcrRegion &region = regions.last();
        region.bounds = region.bounds.isNull() ? word.bounds : region.bounds.united(word.bounds);
        if (region.paragraphs.isEmpty() || word.paragraph != previousParagraph) {
            OcrParagraph paragraph;
            paragraph.id = paragraphId(number, word.block, word.paragraph);
            region.paragraphs.append(paragraph);
            previousLine = -1;
        }
        OcrParagraph &paragraph = region.paragraphs.last();
        paragraph.bounds = paragraph.bounds.isNull() ? word.bounds : paragraph.bounds.united(word.bounds);
        if (paragraph.lines.isEmpty() || word.line != previousLine) {
            OcrLine line;
            line.id = lineId(number, word.block, word.paragraph, word.line);
            paragraph.lines.append(line);
        }
        OcrLine &line = paragraph.lines.last();
        line.bounds = line.bounds.isNull() ? word.bounds : line.bounds.united(word.bounds);
        line.wordIds.append(word.id);
        previousBlock = word.block;
        previousParagraph = word.paragraph;
        previousLine = word.line;
    }
    for (OcrRegion &region : regions) {
        if (const auto old = previousRegions.constFind(region.id); old != previousRegions.cend()) {
            if (!old->bounds.isNull()) region.bounds = old->bounds;
            region.type = old->type;
            region.tableLayout = old->tableLayout;
        }
    }
    QVector<int> heights;
    QHash<QString, int> lineHeights;
    for (const OcrRegion &region : regions)
        for (const OcrParagraph &paragraph : region.paragraphs)
            for (const OcrLine &line : paragraph.lines) {
                heights.append(line.bounds.height());
                lineHeights.insert(line.id, line.bounds.height());
            }
    if (!heights.isEmpty()) {
        std::ranges::sort(heights);
        const int median = qMax(1, heights.at(heights.size() / 2));
        for (OcrWord &word : words) {
            if (word.sourceFontSizePt <= 0)
                word.sourceFontSizePt = qBound(8, qRound(11.0 * lineHeights.value(
                    lineId(number, word.block, word.paragraph, word.line), median) / median), 30);
            if (word.fontSizePt <= 0) word.fontSizePt = word.sourceFontSizePt;
        }
    }
}

bool OcrPage::moveRegion(int index, int direction)
{
    if ((direction != -1 && direction != 1) || index < 0 || index >= regions.size() ||
        index + direction < 0 || index + direction >= regions.size()) return false;
    QVector<QVector<OcrWord>> groups;
    for (const OcrWord &word : words) {
        if (groups.isEmpty() || groups.last().last().block != word.block) groups.emplaceBack();
        groups.last().append(word);
    }
    if (groups.size() != regions.size()) return false;
    groups.swapItemsAt(index, index + direction);
    words.clear();
    for (const auto &group : groups) words += group;
    rebuildHierarchy();
    return true;
}

bool OcrPage::extractRegion(const QVector<int> &wordIndices)
{
    QSet<int> selected;
    for (int index : wordIndices) {
        if (index < 0 || index >= words.size()) return false;
        selected.insert(index);
    }
    if (selected.isEmpty() || selected.size() == words.size()) return false;
    int nextBlock = 0;
    QSet<int> affectedBlocks;
    QVector<OcrWord> kept, moved;
    for (int i = 0; i < words.size(); ++i) {
        const OcrWord &word = words.at(i);
        nextBlock = qMax(nextBlock, word.block);
        if (selected.contains(i)) { moved.append(word); affectedBlocks.insert(word.block); }
        else kept.append(word);
    }
    ++nextBlock;
    for (OcrRegion &region : regions)
        for (int block : affectedBlocks)
            if (region.id == regionId(number, block)) region.bounds = {};
    int nextLine = 0;
    QString previousLine;
    for (OcrWord &word : moved) {
        const QString key = QStringLiteral("%1/%2/%3").arg(word.block).arg(word.paragraph).arg(word.line);
        if (key != previousLine) { ++nextLine; previousLine = key; }
        word.block = nextBlock;
        word.paragraph = 1;
        word.line = nextLine;
        word.id.clear();
    }
    words = kept + moved;
    rebuildHierarchy();
    return true;
}

OcrPage *OcrDocument::page(int number)
{
    for (OcrPage &item : pages) if (item.number == number) return &item;
    return nullptr;
}

const OcrPage *OcrDocument::page(int number) const
{
    for (const OcrPage &item : pages) if (item.number == number) return &item;
    return nullptr;
}

void OcrDocument::replacePage(OcrPage replacement)
{
    replacement.rebuildHierarchy();
    for (OcrPage &item : pages) {
        if (item.number == replacement.number) { item = std::move(replacement); return; }
    }
    pages.append(std::move(replacement));
    std::ranges::sort(pages, [](const OcrPage &a, const OcrPage &b) { return a.number < b.number; });
}

void OcrDocument::removePage(int number)
{
    for (int i = 0; i < pages.size(); ++i)
        if (pages.at(i).number == number) { pages.removeAt(i); return; }
}

qsizetype OcrDocument::wordCount() const
{
    qsizetype count = 0;
    for (const OcrPage &page : pages) count += page.words.size();
    return count;
}

QString OcrDocument::plainText() const
{
    QString output;
    for (const OcrPage &page : pages) {
        if (!output.isEmpty()) output += QStringLiteral("\n\n\f\n\n");
        bool firstWord = true;
        int lastBlock = -1, lastParagraph = -1, lastLine = -1;
        for (const OcrWord &word : page.words) {
            if (!firstWord) {
                if (word.block != lastBlock || word.paragraph != lastParagraph) output += QStringLiteral("\n\n");
                else if (word.line != lastLine) output += QLatin1Char('\n');
                else output += QLatin1Char(' ');
            }
            output += word.text;
            firstWord = false;
            lastBlock = word.block;
            lastParagraph = word.paragraph;
            lastLine = word.line;
        }
    }
    return output;
}

QString OcrDocument::layoutHtml() const
{
    QString html = QStringLiteral("<!doctype html><html><head><meta charset=\"utf-8\"><title>YAGF OCR</title>"
        "<style>body{margin:0;background:#ddd}.page{position:relative;background:white;margin:20px auto;"
        "box-shadow:0 1px 8px #888;page-break-after:always}.word{position:absolute;white-space:pre}</style>"
        "</head><body>");
    for (const OcrPage &page : pages) {
        html += QStringLiteral("<section class=\"page\" style=\"width:%1px;height:%2px\" aria-label=\"Page %3\">")
                    .arg(page.width).arg(page.height).arg(page.number);
        for (const OcrWord &word : page.words) {
            const double sourcePixels = word.sourceFontSizePx > 0 ? word.sourceFontSizePx : word.bounds.height() * 1.15;
            const int visualFontPx = qBound(8, qRound(sourcePixels *
                static_cast<double>(qMax(1, word.fontSizePt)) / qMax(1, word.sourceFontSizePt)), 180);
            html += QStringLiteral("<span class=\"word\" data-word-id=\"%1\" style=\"left:%2px;top:%3px;"
                                   "font-size:%4px;font-weight:%5;font-style:%6;%7\">%8</span>")
                        .arg(word.id.toHtmlEscaped()).arg(word.bounds.x()).arg(word.bounds.y())
                        .arg(visualFontPx)
                        .arg(word.bold ? QStringLiteral("bold") : QStringLiteral("normal"))
                        .arg(word.italic ? QStringLiteral("italic") : QStringLiteral("normal"))
                        .arg(word.fontFamilyCss().toHtmlEscaped())
                        .arg(word.text.toHtmlEscaped());
        }
        html += QStringLiteral("</section>");
    }
    return html + QStringLiteral("</body></html>");
}

QJsonObject OcrDocument::toJson() const
{
    QJsonArray pageItems;
    for (const OcrPage &page : pages) {
        QJsonArray wordItems, regionItems, imageRegionItems, readingOrder, deletedItems, pendingItems;
        for (const auto &word : page.deletedWords) deletedItems.append(wordJson(word));
        for (const auto &word : page.pendingCorrections) pendingItems.append(wordJson(word));
        for (const OcrWord &word : page.words) {
            wordItems.append(wordJson(word));
            readingOrder.append(word.id);
        }
        for (const OcrRegion &region : page.regions) {
            QJsonArray paragraphItems;
            for (const OcrParagraph &paragraph : region.paragraphs) {
                QJsonArray lineItems;
                for (const OcrLine &line : paragraph.lines) {
                    QJsonArray ids;
                    for (const QString &id : line.wordIds) ids.append(id);
                    lineItems.append(QJsonObject{{"id", line.id}, {"box", boxJson(line.bounds)}, {"word_ids", ids}});
                }
                paragraphItems.append(QJsonObject{{"id", paragraph.id}, {"box", boxJson(paragraph.bounds)},
                                                  {"lines", lineItems}});
            }
            QJsonObject item{{"id", region.id}, {"type", region.type}, {"box", boxJson(region.bounds)},
                             {"paragraphs", paragraphItems}};
            if (!region.tableLayout.isEmpty()) item.insert("table_layout", region.tableLayout);
            regionItems.append(item);
        }
        for (const ImageRegion &region : page.imageRegions)
            imageRegionItems.append(QJsonObject{{"id", region.id}, {"box", boxJson(region.bounds)},
                {"language", region.language}, {"not_text", region.notText}});
        pageItems.append(QJsonObject{{"id", page.id}, {"number", page.number},
                                     {"width", page.width}, {"height", page.height},
                                     {"source_width", page.sourceWidth > 0 ? page.sourceWidth : page.width},
                                     {"source_height", page.sourceHeight > 0 ? page.sourceHeight : page.height},
                                     {"render_dpi", page.renderDpi},
                                     {"deleted_words", deletedItems}, {"pending_corrections", pendingItems},
                                     {"preprocessing", QJsonObject{
                                         {"deskew_degrees", page.preprocessing.deskewDegrees},
                                         {"auto_contrast", page.preprocessing.autoContrast},
                                         {"denoise", page.preprocessing.denoise}}},
                                     {"engine", page.engineId}, {"language", page.language},
                                     {"regions", regionItems}, {"image_regions", imageRegionItems},
                                     {"words", wordItems},
                                     {"reading_order", readingOrder}});
    }
    return {{"schema_version", 2}, {"source", sourcePath}, {"engine", engine},
            {"preserve_formatting", preserveFormatting},
            {"rendering", QJsonObject{{"dpi", rendering.dpi}, {"adaptive", rendering.adaptive}}},
            {"language", language}, {"total_pages", totalPages}, {"pages", pageItems},
            {"context", QJsonObject{{"description", context.description},
                                    {"infer_from_recognized_text", context.inferFromRecognizedText}}}};
}

bool OcrDocument::fromJson(const QJsonObject &json, OcrDocument *result, QString *error)
{
    auto fail = [error](const QString &message) { if (error) *error = message; return false; };
    int version = 0;
    if (!integer(json, QStringLiteral("schema_version"), &version) || (version != 1 && version != 2))
        return fail(QStringLiteral("Unsupported OCR document schema."));
    if (!json.value(QStringLiteral("source")).isString() ||
        !json.value(QStringLiteral("engine")).isString() ||
        !json.value(QStringLiteral("language")).isString())
        return fail(QStringLiteral("Invalid OCR document fields."));
    OcrDocument parsed;
    if (json.contains("preserve_formatting") && !json.value("preserve_formatting").isBool())
        return fail(QStringLiteral("Invalid format preservation mode."));
    parsed.preserveFormatting = json.value("preserve_formatting").toBool();
    if (json.contains("rendering")) {
        const auto settings = json.value("rendering");
        if (!settings.isObject() || !integer(settings.toObject(), "dpi", &parsed.rendering.dpi) ||
            !validRenderDpi(parsed.rendering.dpi) || !settings.toObject().value("adaptive").isBool())
            return fail(QStringLiteral("Invalid rendering settings."));
        parsed.rendering.adaptive = settings.toObject().value("adaptive").toBool();
    }
    parsed.sourcePath = json.value(QStringLiteral("source")).toString();
    parsed.engine = json.value(QStringLiteral("engine")).toString();
    parsed.language = json.value(QStringLiteral("language")).toString();
    if (json.contains(QStringLiteral("context"))) {
        if (!json.value(QStringLiteral("context")).isObject())
            return fail(QStringLiteral("Invalid document context."));
        const auto context = json.value(QStringLiteral("context")).toObject();
        if (!context.value(QStringLiteral("description")).isString() ||
            context.value(QStringLiteral("description")).toString().size() > DocumentContext::maximumDescriptionLength ||
            !context.value(QStringLiteral("infer_from_recognized_text")).isBool())
            return fail(QStringLiteral("Invalid document context fields."));
        parsed.context.description = context.value(QStringLiteral("description")).toString();
        parsed.context.inferFromRecognizedText = context.value(QStringLiteral("infer_from_recognized_text")).toBool();
    }
    if (parsed.sourcePath.isEmpty()) return fail(QStringLiteral("Missing source path."));
    QJsonArray pageItems;
    if (version == 1) {
        const QJsonObject legacyPage = json.value(QStringLiteral("page")).toObject();
        pageItems.append(QJsonObject{{"number", 1}, {"width", legacyPage.value(QStringLiteral("width"))},
                                     {"height", legacyPage.value(QStringLiteral("height"))},
                                     {"words", json.value(QStringLiteral("words"))}});
        parsed.totalPages = 1;
    } else {
        if (!integer(json, QStringLiteral("total_pages"), &parsed.totalPages) || parsed.totalPages < 1 ||
            !json.value(QStringLiteral("pages")).isArray())
            return fail(QStringLiteral("Invalid page count or page list."));
        pageItems = json.value(QStringLiteral("pages")).toArray();
    }
    for (const auto &value : pageItems) {
        if (!value.isObject()) return fail(QStringLiteral("Invalid page entry."));
        const QJsonObject object = value.toObject();
        OcrPage page;
        if (!integer(object, QStringLiteral("number"), &page.number) ||
            !integer(object, QStringLiteral("width"), &page.width) ||
            !integer(object, QStringLiteral("height"), &page.height) ||
            page.number < 1 || page.number > parsed.totalPages || page.width <= 0 || page.height <= 0 ||
            !object.value(QStringLiteral("words")).isArray() || parsed.page(page.number))
            return fail(QStringLiteral("Invalid or duplicate page."));
        page.id = object.value(QStringLiteral("id")).toString();
        if (page.id.isEmpty()) page.id = pageId(page.number);
        page.sourceWidth = page.width;
        page.sourceHeight = page.height;
        if (object.contains(QStringLiteral("source_width")) &&
            (!integer(object, QStringLiteral("source_width"), &page.sourceWidth) || page.sourceWidth <= 0))
            return fail(QStringLiteral("Invalid source width."));
        if (object.contains(QStringLiteral("source_height")) &&
            (!integer(object, QStringLiteral("source_height"), &page.sourceHeight) || page.sourceHeight <= 0))
            return fail(QStringLiteral("Invalid source height."));
        if (object.contains(QStringLiteral("preprocessing"))) {
            if (!object.value(QStringLiteral("preprocessing")).isObject())
                return fail(QStringLiteral("Invalid preprocessing settings."));
            const QJsonObject settings = object.value(QStringLiteral("preprocessing")).toObject();
            const QJsonValue angle = settings.value(QStringLiteral("deskew_degrees"));
            const QJsonValue contrast = settings.value(QStringLiteral("auto_contrast"));
            const QJsonValue denoise = settings.value(QStringLiteral("denoise"));
            if (!angle.isDouble() || !std::isfinite(angle.toDouble()) ||
                std::abs(angle.toDouble()) > 15.0 || !contrast.isBool() || !denoise.isBool())
                return fail(QStringLiteral("Invalid preprocessing values."));
            page.preprocessing = {.deskewDegrees = angle.toDouble(),
                                  .autoContrast = contrast.toBool(), .denoise = denoise.toBool()};
        }
        if (object.contains("render_dpi") && (!integer(object, "render_dpi", &page.renderDpi) || !validRenderDpi(page.renderDpi)))
            return fail(QStringLiteral("Invalid page rendering resolution."));
        for (const auto &key : {QStringLiteral("deleted_words"), QStringLiteral("pending_corrections")}) {
            if (!object.contains(key)) continue;
            const auto value = object.value(key);
            if (!value.isArray() || value.toArray().size() > 100000) return fail(QStringLiteral("Invalid saved corrections."));
            auto &destination = key == "deleted_words" ? page.deletedWords : page.pendingCorrections;
            QSet<QString> ids;
            for (const auto &entry : value.toArray()) {
                OcrWord word;
                if (!entry.isObject() || !parseWord(entry.toObject(), page.number, page.width, page.height, &word) ||
                    word.id.isEmpty() || ids.contains(word.id) || !word.reviewed ||
                    (key == "deleted_words" && (!word.text.isEmpty() || word.originalText.isEmpty())))
                    return fail(QStringLiteral("Invalid saved correction."));
                ids.insert(word.id);
                destination.append(std::move(word));
            }
        }
        page.engineId = object.value(QStringLiteral("engine")).toString(parsed.engine);
        page.language = object.value(QStringLiteral("language")).toString(parsed.language);
        if (object.contains(QStringLiteral("image_regions"))) {
            const QJsonValue areas = object.value(QStringLiteral("image_regions"));
            if (!areas.isArray() || areas.toArray().size() > 1000)
                return fail(QStringLiteral("Invalid image regions."));
            QSet<QString> ids;
            for (const auto &area : areas.toArray()) {
                if (!area.isObject()) return fail(QStringLiteral("Invalid image region."));
                const QJsonObject item = area.toObject();
                ImageRegion region;
                if ((item.contains("language") && (!item.value("language").isString() || item.value("language").toString().size() > 64)) ||
                    (item.contains("not_text") && !item.value("not_text").isBool()))
                    return fail(QStringLiteral("Invalid image region language or type."));
                region.language = item.value("language").toString();
                region.notText = item.value("not_text").toBool();
                region.id = item.value(QStringLiteral("id")).toString();
                if (region.id.isEmpty() || ids.contains(region.id) ||
                    !parseBox(item.value(QStringLiteral("box")).toObject(),
                              page.width, page.height, &region.bounds))
                    return fail(QStringLiteral("Invalid image region geometry or ID."));
                ids.insert(region.id);
                page.imageRegions.append(std::move(region));
            }
        }
        for (const auto &wordValue : object.value(QStringLiteral("words")).toArray()) {
            if (!wordValue.isObject()) return fail(QStringLiteral("Invalid word entry."));
            OcrWord word;
            if (!parseWord(wordValue.toObject(), page.number, page.width, page.height, &word))
                return fail(QStringLiteral("Invalid word data or geometry."));
            page.words.append(std::move(word));
        }
        if (version == 2 && object.contains(QStringLiteral("regions"))) {
            if (!object.value(QStringLiteral("regions")).isArray())
                return fail(QStringLiteral("Invalid regions."));
            QSet<QString> regionIds;
            for (const auto &regionValue : object.value(QStringLiteral("regions")).toArray()) {
                if (!regionValue.isObject()) return fail(QStringLiteral("Invalid region entry."));
                const QJsonObject item = regionValue.toObject();
                OcrRegion region;
                region.id = item.value(QStringLiteral("id")).toString();
                region.type = item.value(QStringLiteral("type")).toString();
                if (region.id.isEmpty() || region.type.isEmpty() || regionIds.contains(region.id) ||
                    !parseBox(item.value(QStringLiteral("box")).toObject(), page.width, page.height, &region.bounds))
                    return fail(QStringLiteral("Invalid region geometry or type."));
                if (item.contains("table_layout")) {
                    if (!OcrRegion::validTableLayout(item.value("table_layout")))
                        return fail(QStringLiteral("Invalid table cell layout."));
                    region.tableLayout = item.value("table_layout").toArray();
                }
                regionIds.insert(region.id);
                page.regions.append(std::move(region));
            }
        }
        if (version == 2 && object.contains(QStringLiteral("reading_order"))) {
            const QJsonValue orderValue = object.value(QStringLiteral("reading_order"));
            if (!orderValue.isArray() || orderValue.toArray().size() != page.words.size())
                return fail(QStringLiteral("Invalid reading order."));
            QHash<QString, OcrWord> byId;
            for (const OcrWord &word : page.words) {
                if (word.id.isEmpty() || byId.contains(word.id))
                    return fail(QStringLiteral("Missing or duplicate word ID."));
                byId.insert(word.id, word);
            }
            QVector<OcrWord> ordered;
            QSet<QString> seen;
            for (const auto &idValue : orderValue.toArray()) {
                if (!idValue.isString()) return fail(QStringLiteral("Invalid reading order ID."));
                const QString id = idValue.toString();
                if (!byId.contains(id) || seen.contains(id)) return fail(QStringLiteral("Invalid reading order ID."));
                seen.insert(id);
                ordered.append(byId.value(id));
            }
            page.words = std::move(ordered);
        }
        page.rebuildHierarchy();
        parsed.pages.append(std::move(page));
    }
    if (!result) return fail(QStringLiteral("No output document."));
    *result = std::move(parsed);
    return true;
}

} // namespace yagf::core
