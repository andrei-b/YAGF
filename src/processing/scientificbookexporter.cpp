#include "exportpage.h"

#include <QBuffer>
#include <QHash>
#include <QSaveFile>
#include <QTextStream>

namespace yagf::processing {
namespace {

QString regionText(const core::OcrRegion &region, const QHash<QString, QString> &words)
{
    QStringList paragraphs;
    for (const auto &paragraph : region.paragraphs) {
        QStringList lines;
        for (const auto &line : paragraph.lines) {
            QStringList tokens;
            for (const auto &id : line.wordIds) tokens.append(words.value(id));
            lines.append(tokens.join(' '));
        }
        paragraphs.append(lines.join('\n'));
    }
    return paragraphs.join("\n\n");
}

void writeTable(QTextStream &out, const QString &text, const QJsonArray &layout)
{
    out << "<table>\n";
    const QStringList rows = text.split('\n');
    for (qsizetype i = 0; i < rows.size(); ++i) {
        out << "<tr>";
        const QStringList cells = rows[i].split('\t');
        const auto formats = layout.size() == rows.size() ? layout[i].toArray() : QJsonArray{};
        for (qsizetype j = 0; j < cells.size(); ++j) {
            const auto cell = formats.size() == cells.size() ? formats[j].toObject() : QJsonObject{};
            const QString tag = cell.value("header").toBool() ? "th" : "td";
            out << '<' << tag;
            for (auto key : {"colspan", "rowspan"}) {
                const int span = cell.value(key).toInt(1);
                if (span > 1) out << ' ' << key << "=\"" << span << '"';
            }
            out << '>' << cells[j].toHtmlEscaped() << "</" << tag << '>';
        }
        out << "</tr>\n";
    }
    out << "</table>\n";
}

ExportResult failure(const QString &message)
{
    return {.status = ExportResult::Status::Error, .error = message};
}

} // namespace

ExportResult ScientificBookExporter::write(const core::OcrDocument &document, const QString &destination,
                                           const std::atomic_bool &cancelled, const PdfExporter::Progress &progress)
{
    QSaveFile file(destination);
    if (!file.open(QIODevice::WriteOnly)) return failure(file.errorString());
    QTemporaryDir temporary;
    if (!temporary.isValid()) return failure(QStringLiteral("Could not create temporary export directory."));
    QTextStream out(&file);
    out << "<!doctype html><html><head><meta charset=\"utf-8\"><title>Scientific book</title>"
           "<style>body{max-width:50em;margin:2em auto}img{max-width:100%;height:auto}"
           "p,pre,figcaption,td,th{white-space:pre-wrap}table{border-collapse:collapse}"
           "td,th{border:1px solid;padding:.25em}section{break-after:page}</style></head><body>\n";
    for (qsizetype n = 0; n < document.pages.size(); ++n) {
        const auto &page = document.pages[n];
        QImage image;
        QHash<QString, QString> words;
        for (const auto &word : page.words) words.insert(word.id, word.text);
        out << "<section aria-label=\"Page " << page.number << "\">\n";
        for (qsizetype i = 0; i < page.regions.size(); ++i) {
            if (cancelled.load()) return {.status = ExportResult::Status::Cancelled};
            const auto isIllustration = [&](qsizetype index) {
                return index >= 0 && index < page.regions.size() &&
                    (page.regions[index].isFigure() || page.regions[index].type == "table");
            };
            // Keep an ambiguous caption between two illustrations in its own paragraph.
            const bool leadingCaption = page.regions[i].type == "caption" &&
                !isIllustration(i - 1) && isIllustration(i + 1);
            if (leadingCaption) {
                out << "<figure><figcaption>" << regionText(page.regions[i], words).toHtmlEscaped()
                    << "</figcaption>";
                ++i;
            }
            const auto &region = page.regions[i];
            const bool trailingCaption = !leadingCaption && isIllustration(i) && i + 1 < page.regions.size() &&
                page.regions[i + 1].type == "caption" && !isIllustration(i + 2);
            if (!leadingCaption && (region.isFigure() || trailingCaption)) out << "<figure>";
            const QString text = regionText(region, words);
            if (region.isFigure()) {
                if (image.isNull()) {
                    auto rendered = readExportPage(document, page.number, temporary, cancelled);
                    if (!rendered) return rendered.error();
                    image = std::move(*rendered);
                }
                if (!image.rect().contains(region.bounds))
                    return failure(QStringLiteral("Figure outside page %1.").arg(page.number));
                QBuffer png;
                if (!png.open(QIODevice::WriteOnly) || !image.copy(region.bounds).save(&png, "PNG"))
                    return failure(QStringLiteral("Could not encode figure on page %1.").arg(page.number));
                out << "<img alt=\"" << text.toHtmlEscaped()
                    << "\" src=\"data:image/png;base64," << png.data().toBase64() << "\">";
            } else if (region.type == "table") {
                writeTable(out, text, region.tableLayout);
            } else {
                QString tag = "p";
                if (region.type == "heading" || region.type == "sectionheader") tag = "h2";
                if (region.type == "equation" || region.type == "formula" || region.type == "code") tag = "pre";
                out << '<' << tag << " class=\"" << region.type.toHtmlEscaped() << "\">"
                    << text.toHtmlEscaped() << "</" << tag << ">\n";
            }
            if (trailingCaption)
                out << "<figcaption>" << regionText(page.regions[++i], words).toHtmlEscaped() << "</figcaption>";
            if (region.isFigure() || leadingCaption || trailingCaption) out << "</figure>\n";
        }
        out << "</section>\n";
        if (progress) progress(n + 1, document.pages.size());
    }
    out << "</body></html>\n";
    out.flush();
    if (cancelled.load()) return {.status = ExportResult::Status::Cancelled};
    if (out.status() != QTextStream::Ok || !file.commit()) return failure(file.errorString());
    return {.status = ExportResult::Status::Success};
}

} // namespace yagf::processing
