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
    const core::OcrPage *page = document.page(number);
    int dpi = page ? page->renderDpi : document.rendering.adaptive ? 150 : document.rendering.dpi;
    if (!core::validRenderDpi(dpi)) return std::unexpected(failure(QStringLiteral("Invalid rendering resolution on page %1.").arg(number)));
    QString imagePath = document.sourcePath;
    if (isPagedSource(document.sourcePath)) {
        const QString prefix = temporary.filePath(QStringLiteral("page"));
        const auto [program, arguments, outputPath] = pageRenderCommand(document.sourcePath, number, prefix, false, dpi);
        const auto rendered = detail::runExportCommand(program, arguments, cancelled);
        if (rendered.status != ExportResult::Status::Success) return std::unexpected(rendered);
        imagePath = outputPath;
    }
    QImageReader reader(imagePath);
    reader.setAutoTransform(true);
    QImage source = reader.read();
    if (source.isNull())
        return std::unexpected(failure(QStringLiteral("Could not read page %1: %2").arg(number).arg(reader.errorString())));
    if (!page && document.rendering.adaptive && isPagedSource(document.sourcePath)) {
        dpi = ImageProcessor::recommendedRenderDpi(source);
        while (dpi > 150 && double(source.width()) * source.height() * dpi * dpi / 22500 > 80000000)
            dpi = dpi == 400 ? 300 : dpi == 300 ? 200 : 150;
        if (dpi != 150) {
            const auto command = pageRenderCommand(document.sourcePath, number, temporary.filePath("adaptive-page"), false, dpi);
            const auto rendered = detail::runExportCommand(command.program, command.arguments, cancelled);
            if (rendered.status != ExportResult::Status::Success) return std::unexpected(rendered);
            QImageReader adaptive(command.outputPath); adaptive.setAutoTransform(true); source = adaptive.read();
            if (source.isNull()) return std::unexpected(failure(QStringLiteral("Could not read adaptive page render.")));
        }
    }
    if (page && (source.width() != page->sourceWidth || source.height() != page->sourceHeight))
        return std::unexpected(failure(QStringLiteral("Source dimensions changed on page %1.").arg(number)));
    auto processed = ImageProcessor::apply(source,
        page ? page->preprocessing : core::PreprocessSettings{});
    if (!processed) return std::unexpected(failure(processed.error()));
    if (page && (processed->width() != page->width || processed->height() != page->height))
        return std::unexpected(failure(QStringLiteral("Processed dimensions changed on page %1.").arg(number)));
    const int outputDpi = isPagedSource(document.sourcePath) ? dpi : 150;
    processed->setDotsPerMeterX(qRound(outputDpi / 0.0254));
    processed->setDotsPerMeterY(qRound(outputDpi / 0.0254));
    return std::move(*processed);
}

} // namespace yagf::processing
