#include "pdfexporter.h"

#include "imageprocessor.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QImageReader>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace yagf::processing {
namespace {

PdfExportResult failure(const QString &error)
{
    return {.status = PdfExportResult::Status::Error, .error = error};
}

PdfExportResult runProcess(const QString &program, const QStringList &arguments,
                           const std::atomic_bool &cancelled)
{
    if (QStandardPaths::findExecutable(program).isEmpty())
        return failure(QStringLiteral("%1 is not installed or is not on PATH.").arg(program));
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(10'000))
        return failure(QStringLiteral("Could not start %1: %2").arg(program, process.errorString()));
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning) {
        if (cancelled.load()) {
            process.kill();
            process.waitForFinished(2000);
            return {.status = PdfExportResult::Status::Cancelled};
        }
        if (timer.elapsed() > 300'000) {
            process.kill();
            process.waitForFinished(2000);
            return failure(QStringLiteral("%1 timed out.").arg(program));
        }
        process.waitForFinished(100);
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return failure(QStringLiteral("%1 failed: %2").arg(program,
            QString::fromUtf8(process.readAllStandardError()).trimmed()));
    return {.status = PdfExportResult::Status::Success};
}

void drawWord(QPainter &painter, const core::OcrWord &word)
{
    if (word.text.isEmpty() || word.bounds.isEmpty()) return;
    QFont font = painter.font();
    font.setPixelSize(std::clamp(word.bounds.height(), 6, 180));
    font.setBold(word.bold);
    font.setItalic(word.italic);
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    const qreal naturalWidth = std::max<qreal>(1.0, metrics.horizontalAdvance(word.text));
    const qreal scaleX = static_cast<qreal>(word.bounds.width()) / naturalWidth;
    painter.save();
    painter.translate(word.bounds.x(), word.bounds.y());
    painter.scale(scaleX, 1.0);
    painter.drawText(QPointF(0, metrics.ascent()), word.text);
    painter.restore();
}

QPageSize pageSizeFor(const QImage &image)
{
    constexpr double pointsPerPixel = 72.0 / 150.0;
    return QPageSize(QSizeF(image.width() * pointsPerPixel,
                            image.height() * pointsPerPixel), QPageSize::Point);
}

} // namespace

PdfExportResult PdfExporter::write(const core::OcrDocument &document,
                                   const QString &destination, bool archive,
                                   const std::atomic_bool &cancelled, const Progress &progress)
{
    if (document.sourcePath.isEmpty() || document.totalPages < 1 || destination.isEmpty())
        return failure(QStringLiteral("A source document and destination are required."));
    if (archive && QStandardPaths::findExecutable(QStringLiteral("ocrmypdf")).isEmpty())
        return failure(QStringLiteral("OCRmyPDF is required for PDF/A export."));
    QTemporaryDir temporary;
    if (!temporary.isValid()) return failure(QStringLiteral("Could not create temporary export files."));
    const QString rawPdf = temporary.filePath(QStringLiteral("searchable.pdf"));
    const QString archivePdf = temporary.filePath(QStringLiteral("archive.pdf"));
    const bool sourceIsPdf = document.sourcePath.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive);
    {
        QPdfWriter writer(rawPdf);
        writer.setResolution(150);
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setTitle(QStringLiteral("YAGF searchable document"));
        writer.setCreator(QStringLiteral("YAGF Revival"));
        QPainter painter;
        for (int number = 1; number <= document.totalPages; ++number) {
            if (cancelled.load()) return {.status = PdfExportResult::Status::Cancelled};
            QString imagePath = document.sourcePath;
            if (sourceIsPdf) {
                const QString prefix = temporary.filePath(QStringLiteral("page-%1").arg(number));
                const auto rendered = runProcess(QStringLiteral("pdftoppm"),
                    {QStringLiteral("-f"), QString::number(number),
                     QStringLiteral("-l"), QString::number(number),
                     QStringLiteral("-singlefile"), QStringLiteral("-r"), QStringLiteral("150"),
                     QStringLiteral("-png"), document.sourcePath, prefix}, cancelled);
                if (rendered.status != PdfExportResult::Status::Success) return rendered;
                imagePath = prefix + QStringLiteral(".png");
            }
            QImageReader reader(imagePath);
            reader.setAutoTransform(true);
            const QImage source = reader.read();
            if (source.isNull())
                return failure(QStringLiteral("Could not read page %1: %2").arg(number).arg(reader.errorString()));
            const core::OcrPage *page = document.page(number);
            if (page && (source.width() != page->sourceWidth || source.height() != page->sourceHeight))
                return failure(QStringLiteral("Source dimensions changed on page %1.").arg(number));
            const auto processed = ImageProcessor::apply(source,
                page ? page->preprocessing : core::PreprocessSettings{});
            if (!processed) return failure(processed.error());
            if (page && (processed->width() != page->width || processed->height() != page->height))
                return failure(QStringLiteral("Processed dimensions changed on page %1.").arg(number));
            if (number == 1) {
                writer.setPageSize(pageSizeFor(*processed));
                if (!painter.begin(&writer)) return failure(QStringLiteral("Could not start PDF output."));
            } else {
                writer.setPageSize(pageSizeFor(*processed));
                if (!writer.newPage()) return failure(QStringLiteral("Could not add PDF page %1.").arg(number));
            }
            painter.drawImage(QRect(0, 0, processed->width(), processed->height()), *processed);
            if (page) {
                painter.save();
                painter.setPen(Qt::black);
                painter.setOpacity(0.0);
                for (const core::OcrWord &word : page->words) drawWord(painter, word);
                painter.restore();
            }
            if (progress) progress(number, document.totalPages);
        }
        painter.end();
    }
    if (cancelled.load()) return {.status = PdfExportResult::Status::Cancelled};
    if (!QFileInfo::exists(rawPdf) || QFileInfo(rawPdf).size() == 0)
        return failure(QStringLiteral("PDF output is empty."));
    QString completedPath = rawPdf;
    if (archive) {
        const auto converted = runProcess(QStringLiteral("ocrmypdf"),
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
