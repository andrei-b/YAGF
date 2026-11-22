#include "pdfexporter.h"
#include "exportpage.h"
#include "tesseractengine.h"
#include "detail/exportcommand.h"
#include "detail/pdfpagelayout.h"
#include "detail/pdfpagepainter.h"
#include "detail/pdfformulas.h"
#include "detail/pagebackground.h"

#include <QFile>
#include <QFileInfo>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <cerrno>
#include <cstring>

namespace yagf::processing {
namespace {
ExportResult failure(const QString &error)
{
    return {.status = ExportResult::Status::Error, .error = error};
}

QPageSize pageSizeFor(const QImage &image, int dpiX, int dpiY)
{
    return QPageSize(QSizeF(image.width() * 72.0 / dpiX,
                            image.height() * 72.0 / dpiY), QPageSize::Point);
}

} // namespace

bool PdfExporter::hasPdfLatex()
{
    return !QStandardPaths::findExecutable(QStringLiteral("pdflatex")).isEmpty();
}

PdfExportResult PdfExporter::write(const core::OcrDocument &document,
                                   const QString &destination, bool archive,
                                   const std::atomic_bool &cancelled, const Progress &progress, Mode mode,
                                   FormulaStyle formulas, std::optional<int> onlyPage)
{
    if (document.sourcePath.isEmpty() || document.totalPages < 1 || destination.isEmpty())
        return failure(QStringLiteral("A source document and destination are required."));
    if (onlyPage && (*onlyPage < 1 || *onlyPage > document.totalPages))
        return failure(QStringLiteral("The selected page is outside the source document."));
    const int firstPage = onlyPage.value_or(1);
    const int lastPage = onlyPage.value_or(document.totalPages);
    if (archive && QStandardPaths::findExecutable(QStringLiteral("ocrmypdf")).isEmpty())
        return failure(QStringLiteral("OCRmyPDF is required for PDF/A export."));
    if (mode == Mode::FormattedText && formulas == FormulaStyle::Typeset && !hasPdfLatex())
        formulas = FormulaStyle::Scanned;
    QTemporaryDir temporary;
    if (!temporary.isValid()) return failure(QStringLiteral("Could not create temporary export files."));
    TesseractEngine layoutEngine;
    detail::LayoutRecognizer recognize;
    if (mode == Mode::FormattedText && !QStandardPaths::findExecutable(QStringLiteral("tesseract")).isEmpty()) {
        recognize = [&layoutEngine, &cancelled](const QImage &image, const QString &language, int number)
            -> std::optional<core::OcrPage> {
            auto layout = layoutEngine.recognize(image, {.language = language, .pageNumber = number}, {}, &cancelled);
            if (!layout) return std::nullopt;
            return std::move(*layout);
        };
    }
    QVector<detail::PdfFormulaPage> mathPages;
    bool hasFormulas = false;
    const QString rawPdf = temporary.filePath(QStringLiteral("searchable.pdf"));
    const QString archivePdf = temporary.filePath(QStringLiteral("archive.pdf"));
    {
        QPdfWriter writer(rawPdf);
        writer.setResolution(150);
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setTitle(mode == Mode::ImageOnly ? QStringLiteral("Scanned document") : QStringLiteral("YAGF searchable document"));
        writer.setCreator(QStringLiteral("YAGF 2"));
        QPainter painter;
        for (int number = firstPage; number <= lastPage; ++number) {
            if (cancelled.load()) return {.status = PdfExportResult::Status::Cancelled};
            auto processed = readExportPage(document, number, temporary, cancelled, mode == Mode::ImageOnly);
            if (!processed) return processed.error();
            const int dpi = qRound(processed->dotsPerMeterY() * 0.0254);
            const int dpiY = dpi > 0 ? dpi : 150;
            const int horizontal = qRound(processed->dotsPerMeterX() * 0.0254);
            const int dpiX = horizontal > 0 ? horizontal : dpiY;
            const auto *page = document.page(number);
            if (number == firstPage) {
                writer.setPageSize(pageSizeFor(*processed, dpiX, dpiY));
                if (!painter.begin(&writer)) return failure(QStringLiteral("Could not start PDF output."));
            } else {
                writer.setPageSize(pageSizeFor(*processed, dpiX, dpiY));
                if (!writer.newPage()) return failure(QStringLiteral("Could not add PDF page %1.").arg(number));
            }
            painter.save();
            painter.scale(150.0 / dpiX, 150.0 / dpiY);
            detail::PdfFormulaPage math;
            if (page && mode == Mode::FormattedText) {
                auto layout = detail::preparePdfPage(*page, *processed, cancelled, recognize);
                math = detail::collectPdfFormulas(layout.page,
                    formulas == FormulaStyle::Scanned ? QRegion{} : layout.graphics);
                math.dpiX = dpiX;
                math.dpiY = dpiY;
                if (formulas == FormulaStyle::Scanned) {
                    for (const auto &region : layout.page.regions)
                        if (region.type == "equation" || region.type == "formula") layout.graphics += region.bounds;
                    for (const auto &word : layout.page.words) {
                        if (!math.wordIds.contains(word.id)) continue;
                        layout.graphics += word.bounds;
                        for (const auto &fragment : word.sourceBoxes) layout.graphics += fragment;
                    }
                }
                if (formulas == FormulaStyle::Scanned || math.formulas.isEmpty()) {
                    painter.drawImage(processed->rect(), *processed);
                } else {
                    hasFormulas = true;
                    // Remove the original formula pixels from the embedded background itself.
                    auto background = processed->convertToFormat(QImage::Format_RGB32);
                    QPainter erase(&background);
                    erase.setClipRegion(QRegion(background.rect()).subtracted(layout.graphics));
                    for (const auto &word : layout.page.words) if (math.wordIds.contains(word.id)) {
                        erase.fillRect(word.bounds, detail::paperColor(*processed, word.bounds));
                        for (const auto &fragment : word.sourceBoxes)
                            erase.fillRect(fragment, detail::paperColor(*processed, fragment));
                    }
                    erase.end();
                    painter.drawImage(background.rect(), background);
                }
                if (!detail::drawFormattedPage(painter, layout, *processed, cancelled, math.wordIds)) {
                    painter.restore(); return {.status = PdfExportResult::Status::Cancelled};
                }
            } else {
                painter.drawImage(processed->rect(), *processed);
            }
            if (mode == Mode::FormattedText && formulas == FormulaStyle::Typeset) mathPages.append(std::move(math));
            if (page && mode == Mode::SearchableScan) {
                painter.save();
                painter.setPen(Qt::black);
                painter.setOpacity(0.0);
                const auto formulas = detail::formulaWordIds(*page);
                for (const core::OcrWord &word : page->words)
                    detail::drawWord(painter, word, dpiY, formulas.contains(word.id));
                painter.restore();
            }
            painter.restore();
            if (progress) progress(number - firstPage + 1, lastPage - firstPage + 1);
        }
        painter.end();
    }
    if (cancelled.load()) return {.status = PdfExportResult::Status::Cancelled};
    if (!QFileInfo::exists(rawPdf) || QFileInfo(rawPdf).size() == 0)
        return failure(QStringLiteral("PDF output is empty."));
    QString completedPath = rawPdf;
    if (hasFormulas) {
        const auto result = detail::typesetPdfFormulas(mathPages, temporary, cancelled);
        if (result.status != PdfExportResult::Status::Success) return result;
        completedPath = temporary.filePath("typeset.pdf");
    }
    if (archive) {
        const auto converted = detail::runExportCommand(QStringLiteral("ocrmypdf"),
            {QStringLiteral("--skip-text"), QStringLiteral("--output-type"), QStringLiteral("pdfa"),
             QStringLiteral("--optimize"), QStringLiteral("0"), completedPath, archivePdf}, cancelled);
        if (converted.status != PdfExportResult::Status::Success) return converted;
        completedPath = archivePdf;
    }
    if (cancelled.load()) return {.status = PdfExportResult::Status::Cancelled};
    const QString staged = destination + QLatin1Char('.') +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".tmp");
    if (!QFile::copy(completedPath, staged)) {
        QFile::remove(staged);
        return failure(QStringLiteral("Could not write PDF to %1.").arg(destination));
    }
    if (cancelled.load()) {
        QFile::remove(staged);
        return {.status = PdfExportResult::Status::Cancelled};
    }
    const QByteArray stagedName = QFile::encodeName(staged);
    const QByteArray destinationName = QFile::encodeName(destination);
    if (::rename(stagedName.constData(), destinationName.constData()) != 0) {
        const QString error = QString::fromLocal8Bit(std::strerror(errno));
        QFile::remove(staged);
        return failure(QStringLiteral("Could not complete PDF export: %1").arg(error));
    }
    return {.status = PdfExportResult::Status::Success};
}

} // namespace yagf::processing
