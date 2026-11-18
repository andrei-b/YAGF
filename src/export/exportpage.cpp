#include "exportpage.h"
#include "detail/exportcommand.h"
#include "imageprocessor.h"
#include "pagesource.h"

#include <QImageReader>

namespace yagf::processing {
namespace {
ExportResult failure(const QString &error)
{
    return {.status = ExportResult::Status::Error, .error = error};
}

}

std::expected<QImage, ExportResult> readExportPage(const core::OcrDocument &document, int number,
                                                   const QTemporaryDir &temporary,
                                                   const std::atomic_bool &cancelled)
{
    QString imagePath = document.sourcePath;
    if (isPagedSource(document.sourcePath)) {
        const QString prefix = temporary.filePath(QStringLiteral("page"));
        const auto [program, arguments, outputPath] = pageRenderCommand(document.sourcePath, number, prefix);
        const auto rendered = detail::runExportCommand(program, arguments, cancelled);
        if (rendered.status != ExportResult::Status::Success) return std::unexpected(rendered);
        imagePath = outputPath;
    }
    QImageReader reader(imagePath);
    reader.setAutoTransform(true);
    const QImage source = reader.read();
    if (source.isNull())
        return std::unexpected(failure(QStringLiteral("Could not read page %1: %2").arg(number).arg(reader.errorString())));
    const core::OcrPage *page = document.page(number);
    if (page && (source.width() != page->sourceWidth || source.height() != page->sourceHeight))
        return std::unexpected(failure(QStringLiteral("Source dimensions changed on page %1.").arg(number)));
    auto processed = ImageProcessor::apply(source,
        page ? page->preprocessing : core::PreprocessSettings{});
    if (!processed) return std::unexpected(failure(processed.error()));
    if (page && (processed->width() != page->width || processed->height() != page->height))
        return std::unexpected(failure(QStringLiteral("Processed dimensions changed on page %1.").arg(number)));
    return std::move(*processed);
}

} // namespace yagf::processing
