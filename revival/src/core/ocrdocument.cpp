#include "ocrdocument.h"

#include <QJsonArray>
#include <QHash>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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
    return {{"id", word.id}, {"page", word.page}, {"block", word.block},
            {"paragraph", word.paragraph}, {"line", word.line}, {"box", boxJson(word.bounds)},
            {"original_text", word.originalText}, {"text", word.text},
            {"confidence", word.confidence}, {"reviewed", word.reviewed}};
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
    word->originalText = json.value(QStringLiteral("original_text")).toString();
    word->text = json.value(QStringLiteral("text")).toString();
    word->confidence = json.value(QStringLiteral("confidence")).toDouble();
    word->reviewed = json.value(QStringLiteral("reviewed")).toBool();
    word->id = json.value(QStringLiteral("id")).toString();
    return std::isfinite(word->confidence);
}
}

void OcrPage::rebuildHierarchy()
{
    regions.clear();
    int previousBlock = -1, previousParagraph = -1, previousLine = -1;
    for (int i = 0; i < words.size(); ++i) {
        OcrWord &word = words[i];
        word.page = number;
        if (word.id.isEmpty())
            word.id = QStringLiteral("%1/word-%2").arg(lineId(number, word.block, word.paragraph, word.line)).arg(i + 1);
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
}

OcrDocument OcrDocument::fromTsv(const QByteArray &tsv, int width, int height,
                                 const QString &path, const QString &lang, int pageNumber)
{
    OcrDocument document;
    document.sourcePath = path;
    document.language = lang;
    document.totalPages = pageNumber;
    OcrPage page;
    page.id = pageId(pageNumber);
    page.number = pageNumber;
    page.width = width;
    page.height = height;
    page.engineId = document.engine;
    page.language = lang;
    const QStringList rows = QString::fromUtf8(tsv).split(QLatin1Char('\n'));
    if (!rows.isEmpty() && rows.first().startsWith(QStringLiteral("level\t"))) {
        for (int i = 1; i < rows.size(); ++i) {
            const QStringList fields = rows.at(i).trimmed().split(QLatin1Char('\t'));
            if (fields.size() < 12 || fields.at(0) != QStringLiteral("5")) continue;
            bool ok[8] = {};
            OcrWord word;
            word.page = pageNumber;
            word.block = fields.at(2).toInt(&ok[0]);
            word.paragraph = fields.at(3).toInt(&ok[1]);
            word.line = fields.at(4).toInt(&ok[2]);
            const int x = fields.at(6).toInt(&ok[3]);
            const int y = fields.at(7).toInt(&ok[4]);
            const int w = fields.at(8).toInt(&ok[5]);
            const int h = fields.at(9).toInt(&ok[6]);
            word.confidence = fields.at(10).toDouble(&ok[7]);
            word.originalText = fields.mid(11).join(QLatin1Char('\t')).trimmed();
            word.text = word.originalText;
            bool valid = !word.text.isEmpty() && x >= 0 && y >= 0 && w > 0 && h > 0 &&
                static_cast<qint64>(x) + w <= width && static_cast<qint64>(y) + h <= height;
            for (bool value : ok) valid = valid && value;
            if (!valid) continue;
            word.bounds = QRect(x, y, w, h);
            page.words.append(word);
        }
    }
    page.rebuildHierarchy();
    document.pages.append(std::move(page));
    return document;
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
    std::sort(pages.begin(), pages.end(), [](const OcrPage &a, const OcrPage &b) { return a.number < b.number; });
}

int OcrDocument::wordCount() const
{
    int count = 0;
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

QJsonObject OcrDocument::toJson() const
{
    QJsonArray pageItems;
    for (const OcrPage &page : pages) {
        QJsonArray wordItems, regionItems, readingOrder;
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
            regionItems.append(QJsonObject{{"id", region.id}, {"type", region.type},
                                           {"box", boxJson(region.bounds)}, {"paragraphs", paragraphItems}});
        }
        pageItems.append(QJsonObject{{"id", page.id}, {"number", page.number},
                                     {"width", page.width}, {"height", page.height},
                                     {"engine", page.engineId}, {"language", page.language},
                                     {"regions", regionItems}, {"words", wordItems},
                                     {"reading_order", readingOrder}});
    }
    return {{"schema_version", 2}, {"source", sourcePath}, {"engine", engine},
            {"language", language}, {"total_pages", totalPages}, {"pages", pageItems}};
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
    parsed.sourcePath = json.value(QStringLiteral("source")).toString();
    parsed.engine = json.value(QStringLiteral("engine")).toString();
    parsed.language = json.value(QStringLiteral("language")).toString();
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
    for (const QJsonValue &value : pageItems) {
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
        page.engineId = object.value(QStringLiteral("engine")).toString(parsed.engine);
        page.language = object.value(QStringLiteral("language")).toString(parsed.language);
        for (const QJsonValue &wordValue : object.value(QStringLiteral("words")).toArray()) {
            if (!wordValue.isObject()) return fail(QStringLiteral("Invalid word entry."));
            OcrWord word;
            if (!parseWord(wordValue.toObject(), page.number, page.width, page.height, &word))
                return fail(QStringLiteral("Invalid word data or geometry."));
            page.words.append(std::move(word));
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
            for (const QJsonValue &idValue : orderValue.toArray()) {
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
