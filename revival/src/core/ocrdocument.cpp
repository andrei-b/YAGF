#include "ocrdocument.h"

#include <QJsonArray>
#include <QStringList>

#include <cmath>
#include <limits>
#include <utility>

namespace {
bool integer(const QJsonObject &object, const QString &key, int *value)
{
    const QJsonValue field = object.value(key);
    if (!field.isDouble()) return false;
    const double number = field.toDouble();
    if (!std::isfinite(number) || number != std::floor(number) ||
        number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) return false;
    *value = static_cast<int>(number);
    return true;
}
}

OcrDocument OcrDocument::fromTsv(const QByteArray &tsv, int pageWidth, int pageHeight,
                                 const QString &path, const QString &lang)
{
    OcrDocument document;
    document.sourcePath = path;
    document.language = lang;
    document.width = pageWidth;
    document.height = pageHeight;
    const QStringList rows = QString::fromUtf8(tsv).split(QLatin1Char('\n'));
    if (rows.isEmpty() || !rows.first().startsWith(QStringLiteral("level\t")))
        return document;
    for (int i = 1; i < rows.size(); ++i) {
        const QStringList fields = rows.at(i).trimmed().split(QLatin1Char('\t'));
        if (fields.size() < 12 || fields.at(0) != QStringLiteral("5"))
            continue;
        bool ok[7] = {};
        const int page = fields.at(1).toInt(&ok[0]);
        const int block = fields.at(2).toInt(&ok[1]);
        const int paragraph = fields.at(3).toInt(&ok[2]);
        const int line = fields.at(4).toInt(&ok[3]);
        const int left = fields.at(6).toInt(&ok[4]);
        const int top = fields.at(7).toInt(&ok[5]);
        const int width = fields.at(8).toInt(&ok[6]);
        bool heightOk = false;
        const int height = fields.at(9).toInt(&heightOk);
        bool confidenceOk = false;
        const double confidence = fields.at(10).toDouble(&confidenceOk);
        const QString wordText = fields.mid(11).join(QLatin1Char('\t')).trimmed();
        bool valid = heightOk && confidenceOk && width > 0 && height > 0 && !wordText.isEmpty();
        for (bool value : ok)
            valid = valid && value;
        if (!valid || left < 0 || top < 0 || left + width > pageWidth || top + height > pageHeight)
            continue;
        document.words.append({page, block, paragraph, line, QRect(left, top, width, height),
                               wordText, wordText, confidence, false});
    }
    return document;
}

bool OcrDocument::fromJson(const QJsonObject &json, OcrDocument *result, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    int version = 0;
    if (!integer(json, QStringLiteral("schema_version"), &version) || version != 1)
        return fail(QStringLiteral("Unsupported OCR document schema."));
    if (!json.value(QStringLiteral("source")).isString() ||
        !json.value(QStringLiteral("engine")).isString() ||
        !json.value(QStringLiteral("language")).isString() ||
        !json.value(QStringLiteral("page")).isObject() ||
        !json.value(QStringLiteral("words")).isArray())
        return fail(QStringLiteral("Invalid OCR document fields."));
    OcrDocument parsed;
    parsed.sourcePath = json.value(QStringLiteral("source")).toString();
    parsed.engine = json.value(QStringLiteral("engine")).toString();
    parsed.language = json.value(QStringLiteral("language")).toString();
    const QJsonObject page = json.value(QStringLiteral("page")).toObject();
    if (parsed.sourcePath.isEmpty() || !integer(page, QStringLiteral("width"), &parsed.width) ||
        !integer(page, QStringLiteral("height"), &parsed.height) || parsed.width <= 0 || parsed.height <= 0)
        return fail(QStringLiteral("Invalid source or page size."));
    const QJsonArray items = json.value(QStringLiteral("words")).toArray();
    for (const QJsonValue &item : items) {
        if (!item.isObject()) return fail(QStringLiteral("Invalid word entry."));
        const QJsonObject object = item.toObject();
        const QJsonObject box = object.value(QStringLiteral("box")).toObject();
        OcrWord word;
        int x = 0, y = 0, width = 0, height = 0;
        if (!integer(object, QStringLiteral("page"), &word.page) ||
            !integer(object, QStringLiteral("block"), &word.block) ||
            !integer(object, QStringLiteral("paragraph"), &word.paragraph) ||
            !integer(object, QStringLiteral("line"), &word.line) ||
            !integer(box, QStringLiteral("x"), &x) || !integer(box, QStringLiteral("y"), &y) ||
            !integer(box, QStringLiteral("width"), &width) || !integer(box, QStringLiteral("height"), &height) ||
            !object.value(QStringLiteral("original_text")).isString() ||
            !object.value(QStringLiteral("text")).isString() ||
            !object.value(QStringLiteral("confidence")).isDouble() ||
            !object.value(QStringLiteral("reviewed")).isBool() ||
            word.page != 1 || width <= 0 || height <= 0 || x < 0 || y < 0 ||
            static_cast<qint64>(x) + width > parsed.width ||
            static_cast<qint64>(y) + height > parsed.height)
            return fail(QStringLiteral("Invalid word data or geometry."));
        word.bounds = QRect(x, y, width, height);
        word.originalText = object.value(QStringLiteral("original_text")).toString();
        word.text = object.value(QStringLiteral("text")).toString();
        word.confidence = object.value(QStringLiteral("confidence")).toDouble();
        if (!std::isfinite(word.confidence)) return fail(QStringLiteral("Invalid confidence value."));
        word.reviewed = object.value(QStringLiteral("reviewed")).toBool();
        parsed.words.append(word);
    }
    if (!result) return fail(QStringLiteral("No output document."));
    *result = std::move(parsed);
    return true;
}

QString OcrDocument::plainText() const
{
    QString output;
    int lastPage = -1, lastBlock = -1, lastParagraph = -1, lastLine = -1;
    for (const OcrWord &word : words) {
        if (!output.isEmpty()) {
            if (word.page != lastPage || word.block != lastBlock || word.paragraph != lastParagraph)
                output += QStringLiteral("\n\n");
            else if (word.line != lastLine)
                output += QLatin1Char('\n');
            else
                output += QLatin1Char(' ');
        }
        output += word.text;
        lastPage = word.page;
        lastBlock = word.block;
        lastParagraph = word.paragraph;
        lastLine = word.line;
    }
    return output;
}

QJsonObject OcrDocument::toJson() const
{
    QJsonArray items;
    for (const OcrWord &word : words) {
        QJsonObject box{{"x", word.bounds.x()}, {"y", word.bounds.y()},
                        {"width", word.bounds.width()}, {"height", word.bounds.height()}};
        items.append(QJsonObject{{"page", word.page}, {"block", word.block},
                                 {"paragraph", word.paragraph}, {"line", word.line},
                                 {"box", box}, {"original_text", word.originalText},
                                 {"text", word.text}, {"confidence", word.confidence},
                                 {"reviewed", word.reviewed}, {"engine", engine}});
    }
    return QJsonObject{{"schema_version", 1}, {"source", sourcePath},
                       {"engine", engine}, {"language", language},
                       {"page", QJsonObject{{"width", width}, {"height", height}}},
                       {"words", items}};
}
