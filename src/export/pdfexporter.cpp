#include "pdfexporter.h"
#include "exportpage.h"
#include "tesseractengine.h"
#include "detail/exportcommand.h"
#include "detail/pdfpagelayout.h"
#include "detail/pdfpagepainter.h"

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

PdfExportResult PdfExporter::write(const core::OcrDocument &document,
                                   const QString &destination, bool archive,
                                   const std::atomic_bool &cancelled, const Progress &progress, Mode mode)
{
    if (document.sourcePath.isEmpty() || document.totalPages < 1 || destination.isEmpty())
        return failure(QStringLiteral("A source document and destination are required."));
    if (archive && QStandardPaths::findExecutable(QStringLiteral("ocrmypdf")).isEmpty())
        return failure(QStringLiteral("OCRmyPDF is required for PDF/A export."));
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
    const QString rawPdf = temporary.filePath(QStringLiteral("searchable.pdf"));
    const QString archivePdf = temporary.filePath(QStringLiteral("archive.pdf"));
    {
        QPdfWriter writer(rawPdf);
        writer.setResolution(150);
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setTitle(mode == Mode::ImageOnly ? QStringLiteral("Scanned document") : QStringLiteral("YAGF searchable document"));
        writer.setCreator(QStringLiteral("YAGF 2"));
        QPainter painter;
        for (int number = 1; number <= document.totalPages; ++number) {
            if (cancelled.load()) return {.status = PdfExportResult::Status::Cancelled};
            auto processed = readExportPage(document, number, temporary, cancelled, mode == Mode::ImageOnly);
            if (!processed) return processed.error();
            const int dpi = qRound(processed->dotsPerMeterY() * 0.0254);
            const int dpiY = dpi > 0 ? dpi : 150;
            const int horizontal = qRound(processed->dotsPerMeterX() * 0.0254);
            const int dpiX = horizontal > 0 ? horizontal : dpiY;
            const auto *page = document.page(number);
            if (number == 1) {
                writer.setPageSize(pageSizeFor(*processed, dpiX, dpiY));
                if (!painter.begin(&writer)) return failure(QStringLiteral("Could not start PDF output."));
            } else {
                writer.setPageSize(pageSizeFor(*processed, dpiX, dpiY));
                if (!writer.newPage()) return failure(QStringLiteral("Could not add PDF page %1.").arg(number));
            }
            painter.save();
            painter.scale(150.0 / dpiX, 150.0 / dpiY);
            painter.drawImage(QRect(0, 0, processed->width(), processed->height()), *processed);
            if (page && mode == Mode::FormattedText) {
                const auto layout = detail::preparePdfPage(*page, *processed, cancelled, recognize);
                if (!detail::drawFormattedPage(painter, layout, *processed, cancelled)) {
                    painter.restore(); return {.status = PdfExportResult::Status::Cancelled};
                }
            } else if (page && mode == Mode::SearchableScan) {
                painter.save();
                painter.setPen(Qt::black);
                painter.setOpacity(0.0);
                const auto formulas = detail::formulaWordIds(*page);
                for (const core::OcrWord &word : page->words)
                    detail::drawWord(painter, word, dpiY, formulas.contains(word.id));
                painter.restore();
            }
            painter.restore();
            if (progress) progress(number, document.totalPages);
        }
        painter.end();
    }
    if (cancelled.load()) return {.status = PdfExportResult::Status::Cancelled};
    if (!QFileInfo::exists(rawPdf) || QFileInfo(rawPdf).size() == 0)
        return failure(QStringLiteral("PDF output is empty."));
    QString completedPath = rawPdf;
    if (archive) {
        const auto converted = detail::runExportCommand(QStringLiteral("ocrmypdf"),
            {QStringLiteral("--skip-text"), QStringLiteral("--output-type"), QStringLiteral("pdfa"),
             QStringLiteral("--optimize"), QStringLiteral("0"), rawPdf, archivePdf}, cancelled);
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
