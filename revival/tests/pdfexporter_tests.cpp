#include "pdfexporter.h"

#include <QGuiApplication>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QFile>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using yagf::core::OcrDocument;
using yagf::processing::PdfExportResult;
using yagf::processing::PdfExporter;

static OcrDocument sampleDocument(int width, int height, const QString &path, int number = 1)
{
    OcrDocument document;
    document.sourcePath = path;
    document.language = QStringLiteral("eng");
    document.totalPages = number;
    yagf::core::OcrPage page;
    page.number = number;
    page.width = page.sourceWidth = width;
    page.height = page.sourceHeight = height;
    page.language = QStringLiteral("eng");
    const QRect box(2, 2, qMin(110, width - 2), qMin(30, height - 2));
    page.words.append({number, 1, 1, 1, box,
                       QStringLiteral("Mistake"), QStringLiteral("Mistake"), 90});
    document.replacePage(std::move(page));
    return document;
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir directory;
    assert(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("source.png"));
    QImage image(400, 120, QImage::Format_RGB32);
    image.fill(Qt::white);
    assert(image.save(imagePath));
    OcrDocument document = sampleDocument(400, 120, imagePath);
    document.page(1)->words[0].text = QStringLiteral("Corrected");
    const QString pdfPath = directory.filePath(QStringLiteral("output.pdf"));
    std::atomic_bool cancelled = false;
    const auto result = PdfExporter::write(document, pdfPath, false, cancelled);
    assert(result.status == PdfExportResult::Status::Success);
    QProcess extract;
    extract.start(QStringLiteral("pdftotext"), {pdfPath, QStringLiteral("-")});
    assert(extract.waitForFinished(10'000));
    assert(extract.exitCode() == 0);
    const QByteArray text = extract.readAllStandardOutput();
    assert(text.contains("Corrected"));
    assert(!text.contains("Mistake"));
    // A phone photo with EXIF orientation must keep the same geometry in OCR and export.
    const QString photoPath = directory.filePath(QStringLiteral("rotated.jpg"));
    QImage photo(40, 20, QImage::Format_RGB32);
    photo.fill(Qt::white);
    assert(photo.save(photoPath, "JPEG"));
    QFile photoFile(photoPath);
    assert(photoFile.open(QIODevice::ReadOnly));
    const QByteArray jpeg = photoFile.readAll();
    photoFile.close();
    const QByteArray exif = QByteArray::fromHex(
        "FFE100224578696600004D4D002A00000008000101120003000000010006000000000000");
    assert(jpeg.startsWith(QByteArray::fromHex("FFD8")));
    assert(photoFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    assert(photoFile.write(jpeg.left(2) + exif + jpeg.mid(2)) > 0);
    photoFile.close();
    QImageReader photoReader(photoPath);
    photoReader.setAutoTransform(true);
    assert(photoReader.read().size() == QSize(20, 40));
    OcrDocument oriented = sampleDocument(20, 40, photoPath);
    assert(PdfExporter::write(oriented, directory.filePath(QStringLiteral("rotated.pdf")),
                              false, cancelled).status == PdfExportResult::Status::Success);
    if (!QStandardPaths::findExecutable(QStringLiteral("ocrmypdf")).isEmpty()) {
        const QString archivePath = directory.filePath(QStringLiteral("archive.pdf"));
        const auto archived = PdfExporter::write(document, archivePath, true, cancelled);
        assert(archived.status == PdfExportResult::Status::Success);
        extract.start(QStringLiteral("pdftotext"), {archivePath, QStringLiteral("-")});
        assert(extract.waitForFinished(10'000));
        assert(extract.exitCode() == 0);
        assert(extract.readAllStandardOutput().contains("Corrected"));
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("pdftoppm")).isEmpty()) {
        const QString inputPdf = directory.filePath(QStringLiteral("input.pdf"));
        {
            QPdfWriter writer(inputPdf);
            writer.setResolution(150);
            writer.setPageMargins(QMarginsF(0, 0, 0, 0));
            writer.setPageSize(QPageSize(QSizeF(400 * 72.0 / 150, 120 * 72.0 / 150), QPageSize::Point));
            QPainter painter(&writer);
            painter.fillRect(QRect(0, 0, 400, 120), Qt::white);
            writer.setPageSize(QPageSize(QSizeF(300 * 72.0 / 150, 200 * 72.0 / 150), QPageSize::Point));
            assert(writer.newPage());
            painter.fillRect(QRect(0, 0, 300, 200), Qt::white);
            painter.end();
        }
        QProcess render;
        const QString firstPrefix = directory.filePath(QStringLiteral("first"));
        const QString secondPrefix = directory.filePath(QStringLiteral("second"));
        for (int number = 1; number <= 2; ++number) {
            render.start(QStringLiteral("pdftoppm"),
                {QStringLiteral("-f"), QString::number(number), QStringLiteral("-l"), QString::number(number),
                 QStringLiteral("-singlefile"), QStringLiteral("-r"), QStringLiteral("150"),
                 QStringLiteral("-png"), inputPdf, number == 1 ? firstPrefix : secondPrefix});
            assert(render.waitForFinished(10'000));
            assert(render.exitCode() == 0);
        }
        const QImage first(firstPrefix + QStringLiteral(".png"));
        const QImage second(secondPrefix + QStringLiteral(".png"));
        assert(!first.isNull() && !second.isNull());
        OcrDocument pages = sampleDocument(first.width(), first.height(), inputPdf);
        pages.page(1)->words[0].text = QStringLiteral("FirstFix");
        OcrDocument next = sampleDocument(second.width(), second.height(), inputPdf, 2);
        next.page(2)->words[0].text = QStringLiteral("SecondFix");
        pages.totalPages = 2;
        pages.replacePage(*next.page(2));
        const QString multiPdf = directory.filePath(QStringLiteral("multi.pdf"));
        assert(PdfExporter::write(pages, multiPdf, false, cancelled).status == PdfExportResult::Status::Success);
        extract.start(QStringLiteral("pdftotext"), {multiPdf, QStringLiteral("-")});
        assert(extract.waitForFinished(10'000));
        assert(extract.exitCode() == 0);
        const QByteArray multiText = extract.readAllStandardOutput();
        assert(multiText.contains("FirstFix"));
        assert(multiText.contains("SecondFix"));
        std::atomic_bool stopAfterFirst = false;
        const QString cancelledPath = directory.filePath(QStringLiteral("cancelled.pdf"));
        const auto interrupted = PdfExporter::write(pages, cancelledPath, false, stopAfterFirst,
            [&stopAfterFirst](int completed, int) { if (completed == 1) stopAfterFirst = true; });
        assert(interrupted.status == PdfExportResult::Status::Cancelled);
        assert(!QFileInfo::exists(cancelledPath));
    }
    cancelled = true;
    assert(PdfExporter::write(document, pdfPath, false, cancelled).status == PdfExportResult::Status::Cancelled);
}
