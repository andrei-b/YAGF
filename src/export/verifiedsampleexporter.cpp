#include "verifiedsampleexporter.h"
#include "exportpage.h"
#include "correctionmemory.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <optional>

namespace yagf::processing {
namespace {
ExportResult failure(const QString &error)
{
    return {.status = ExportResult::Status::Error, .error = error};
}

}

ExportResult VerifiedSampleExporter::write(const core::OcrDocument &document, const QString &destination,
                                       const std::atomic_bool &cancelled, const ExportProgress &progress)
{
    QTemporaryDir temporary;
    if (!temporary.isValid()) return failure(QStringLiteral("Could not create temporary export files."));
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) return failure(output.errorString());
    int count = 0;
    int completed = 0;
    for (const auto &page : document.pages) {
        std::optional<QImage> image;
        for (const auto &word : page.words) {
            if (cancelled.load()) return {.status = ExportResult::Status::Cancelled};
            if (!core::isVerifiedCorrection(word)) continue;
            if (!image) {
                auto loaded = readExportPage(document, page.number, temporary, cancelled);
                if (!loaded) return loaded.error();
                image = std::move(*loaded);
            }
            if (!image->rect().contains(word.bounds))
                return failure(QStringLiteral("Word outside the image on page %1.").arg(page.number));
            QByteArray png;
            QBuffer buffer(&png);
            if (!buffer.open(QIODevice::WriteOnly) || !image->copy(word.bounds).save(&buffer, "PNG"))
                return failure(QStringLiteral("Could not encode a word on page %1.").arg(page.number));
            const QJsonObject sample{
                {"page", page.number}, {"word_id", word.id}, {"source_word_id", word.sourceWordIds.first()},
                {"engine", page.engineId}, {"language", page.language}, {"confidence", word.confidence},
                {"box", QJsonArray{word.bounds.x(), word.bounds.y(), word.bounds.width(), word.bounds.height()}},
                {"original_text", word.originalText}, {"text", word.text},
                {"png_base64", QString::fromLatin1(png.toBase64())}
            };
            const QByteArray row = QJsonDocument(sample).toJson(QJsonDocument::Compact) + '\n';
            if (output.write(row) != row.size()) return failure(output.errorString());
            ++count;
        }
        ++completed;
        if (progress) progress(completed, document.pages.size());
    }
    if (cancelled.load()) return {.status = ExportResult::Status::Cancelled};
    if (!count) return failure(QStringLiteral("No verified single-word corrections to export."));
    if (!output.commit()) return failure(output.errorString());
    return {.status = ExportResult::Status::Success};
}

} // namespace yagf::processing
