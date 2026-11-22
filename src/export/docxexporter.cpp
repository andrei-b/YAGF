#include "docxexporter.h"
#include "detail/exportcommand.h"
#include "latexdocumentexporter.h"

#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace yagf::processing {

ExportResult DocxExporter::write(const core::OcrDocument &document, const QString &destination,
                                 const std::atomic_bool &cancelled, const ExportProgress &progress)
{
    const auto failure = [](const QString &error) {
        return ExportResult{.status = ExportResult::Status::Error, .error = error};
    };
    if (cancelled.load()) return {.status = ExportResult::Status::Cancelled};
    if (QStandardPaths::findExecutable("pandoc").isEmpty())
        return failure(QStringLiteral("Install Pandoc to export a Word document (.docx)."));

    QTemporaryDir work;
    if (!work.isValid()) return failure(work.errorString());
    const auto prepared = LatexDocumentExporter::write(document, work.filePath("document.tex"), cancelled, progress);
    if (prepared.status != ExportResult::Status::Success) return prepared;

    if (!QFile::copy(":/export/docx.lua", work.filePath("docx.lua")))
        return failure(QStringLiteral("Could not prepare the Word export filter."));
    const auto converted = detail::runExportCommand("pandoc",
        {"--from=latex+raw_tex", "--to=docx", "--fail-if-warnings", "--lua-filter=docx.lua", "--output=document.docx", "document.tex"},
        cancelled, work.path());
    if (converted.status != ExportResult::Status::Success) return converted;

    QFile source(work.filePath("document.docx"));
    if (!source.open(QIODevice::ReadOnly)) return failure(source.errorString());
    if (source.size() == 0) return failure(QStringLiteral("Pandoc produced an empty Word document."));
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) return failure(output.errorString());
    char buffer[64 * 1024];
    while (!source.atEnd()) {
        if (cancelled.load()) return {.status = ExportResult::Status::Cancelled};
        const auto size = source.read(buffer, sizeof buffer);
        if (size <= 0) return failure(source.errorString());
        if (output.write(buffer, size) != size) return failure(output.errorString());
    }
    if (cancelled.load()) return {.status = ExportResult::Status::Cancelled};
    if (!output.commit()) return failure(output.errorString());
    return {.status = ExportResult::Status::Success};
}

} // namespace yagf::processing
