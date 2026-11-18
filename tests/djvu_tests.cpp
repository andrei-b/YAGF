#include "documentprocessor.h"
#include "exportpage.h"
#include "pagesource.h"
#include "projectfile.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QMap>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf;

static QByteArray run(const QString &program, const QStringList &arguments)
{
    QProcess process;
    process.start(program, arguments);
    assert(process.waitForFinished(10000));
    if (process.exitCode()) std::fprintf(stderr, "%s\n", process.readAllStandardError().constData());
    assert(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0);
    return process.readAllStandardOutput();
}

template<typename Condition> static void waitFor(Condition condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    assert(condition());
}

class FixtureEngine final : public processing::IOcrEngine {
public:
    QString id() const override { return "fixture"; }
    processing::EngineCapabilities capabilities() const override { return {.supportsWordBoxes = true}; }
    std::expected<core::OcrPage, processing::OcrError> recognize(const QImage &image,
        const processing::OcrOptions &options, std::stop_token) override
    {
        core::OcrPage page;
        page.number = options.pageNumber;
        page.width = page.sourceWidth = image.width();
        page.height = page.sourceHeight = image.height();
        page.language = options.language;
        const QString text = options.pageNumber == 1 ? "FirstFix" : "SecondFix";
        page.words.append({options.pageNumber, 1, 1, 1, QRect(10, 10, 110, 25), text, text, 97});
        return page;
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    assert(processing::pageSourceKind("book.DJV") == processing::PageSourceKind::DjVu);
    assert(processing::pageSourceKind("book.DjVu") == processing::PageSourceKind::DjVu);
    assert(processing::isPagedSource("book.PDF") && !processing::isPagedSource("scan.png"));
    for (const auto tool : {"c44", "djvm", "djvused", "ddjvu", "pdfinfo", "pdftotext"})
        if (QStandardPaths::findExecutable(tool).isEmpty()) {
            std::printf("Skipping real DjVu integration: %s is not installed.\n", tool);
            return 77;
        }
    QTemporaryDir directory;
    assert(directory.isValid());
    const QSize sizes[]{QSize(400, 240), QSize(300, 400)};
    for (int page = 1; page <= 2; ++page) {
        QImage image(sizes[page - 1], QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        painter.drawText(20, 40, QStringLiteral("DjVu page %1").arg(page));
        painter.end();
        const QString ppm = directory.filePath(QStringLiteral("page%1.ppm").arg(page));
        assert(image.save(ppm, "PPM"));
        run("c44", {"-dpi", "150", ppm, directory.filePath(QStringLiteral("page%1.djvu").arg(page))});
    }
    const QString source = directory.filePath("source book.DJV");
    run("djvm", {"-c", source, directory.filePath("page1.djvu"), directory.filePath("page2.djvu")});
    // The backend must be selected before asynchronous paged-document loading begins.
    QProcess desktop;
    desktop.start(QString::fromUtf8(YAGF_APP_PATH), {"--paddleocr-vl-url", "http://127.0.0.1:8765", source});
    assert(desktop.waitForStarted(5000));
    assert(!desktop.waitForFinished(1000) && desktop.state() == QProcess::Running);
    desktop.terminate();
    assert(desktop.waitForFinished(5000));
    const QByteArray originalSource = [&] { QFile file(source); assert(file.open(QIODevice::ReadOnly)); return file.readAll(); }();
    processing::DocumentProcessor processor;
    int count = 0;
    QString error;
    QMap<int, QImage> rendered, thumbnails;
    QObject::connect(&processor, &processing::DocumentProcessor::failed, &app, [&](const QString &message) { error = message; });
    QObject::connect(&processor, &processing::DocumentProcessor::documentOpened, &app, [&](int pages) { count = pages; });
    QObject::connect(&processor, &processing::DocumentProcessor::pageRendered, &app,
        [&](int page, const QString &path) { rendered.insert(page, QImage(path)); });
    QObject::connect(&processor, &processing::DocumentProcessor::thumbnailReady, &app,
        [&](int page, const QString &path) { thumbnails.insert(page, QImage(path)); });
    processor.openDocument(source);
    waitFor([&] { return count || !error.isEmpty(); });
    assert(error.isEmpty() && count == 2);
    processor.startThumbnails(count);
    processor.renderDocumentPage(1);
    waitFor([&] { return rendered.contains(1) && thumbnails.size() == 2; });
    processor.renderDocumentPage(2);
    waitFor([&] { return rendered.contains(2); });
    for (int page = 1; page <= 2; ++page) {
        assert(rendered[page].size() == sizes[page - 1]);
        assert(!thumbnails[page].isNull());
        assert(thumbnails[page].width() <= 160 && thumbnails[page].height() <= 160);
    }
    // Plain conversion requires no recognized pages.
    core::OcrDocument document;
    document.sourcePath = source;
    document.totalPages = 2;
    std::atomic_bool cancelled = false;
    const QString plainPdf = directory.filePath("converted.pdf");
    assert(processing::PdfExporter::write(document, plainPdf, false, cancelled).status == processing::ExportResult::Status::Success);
    assert(run("pdfinfo", {plainPdf}).contains("Pages:           2"));

    // Batch decoding and OCR use the same page geometry as preview and export.
    processing::DocumentProcessor batch;
    batch.setEngine(std::make_unique<FixtureEngine>());
    int opened = 0;
    bool complete = false;
    QObject::connect(&batch, &processing::DocumentProcessor::documentOpened, &app, [&](int pages) { opened = pages; });
    QObject::connect(&batch, &processing::DocumentProcessor::batchPageRecognized, &app,
        [&](core::OcrPage page, QSize sourceSize) {
            assert(sourceSize == sizes[page.number - 1]);
            document.replacePage(std::move(page));
        });
    QObject::connect(&batch, &processing::DocumentProcessor::batchFinished, &app, [&] { complete = true; });
    batch.openDocument(source);
    waitFor([&] { return opened == 2; });
    batch.recognizePages({{1, "eng", {}}, {2, "eng", {}}});
    waitFor([&] { return complete; });
    assert(document.pages.size() == 2 && document.wordCount() == 2);
    const QString searchablePdf = directory.filePath("searchable.pdf");
    assert(processing::PdfExporter::write(document, searchablePdf, false, cancelled).status == processing::ExportResult::Status::Success);
    const auto text = run("pdftotext", {searchablePdf, "-"});
    assert(text.contains("FirstFix") && text.contains("SecondFix"));
    QFile saved(searchablePdf);
    assert(saved.open(QIODevice::ReadOnly));
    const auto bytes = saved.readAll();
    saved.close();
    const auto interrupted = processing::PdfExporter::write(document, searchablePdf, false, cancelled,
        [&](int page, int) { if (page == 1) cancelled = true; });
    assert(interrupted.status == processing::ExportResult::Status::Cancelled);
    assert(saved.open(QIODevice::ReadOnly) && saved.readAll() == bytes);
    cancelled = false;

    const QString project = directory.filePath("book.yagf.json");
    assert(core::ProjectFile::save(project, document, &error));
    core::OcrDocument restored;
    assert(core::ProjectFile::load(project, &restored, &error));
    assert(restored.sourcePath == source && restored.plainText() == document.plainText());
    const QString portable = directory.filePath("portable");
    assert(core::ProjectFile::savePortable(portable, restored, &error));
    assert(core::ProjectFile::loadPortable(portable, &restored, &error));
    QTemporaryDir exportedPage;
    const auto restoredImage = processing::readExportPage(restored, 2, exportedPage, cancelled);
    assert(restoredImage && restoredImage->size() == sizes[1]);

    // Cancelling a decode must discard a partial image instead of caching it.
    processing::DocumentProcessor interruptedRender;
    bool ready = false, stopped = false;
    QImage retried;
    QObject::connect(&interruptedRender, &processing::DocumentProcessor::documentOpened, &app, [&](int) { ready = true; });
    QObject::connect(&interruptedRender, &processing::DocumentProcessor::cancelled, &app, [&] { stopped = true; });
    QObject::connect(&interruptedRender, &processing::DocumentProcessor::pageRendered, &app,
        [&](int, const QString &path) { retried.load(path); });
    interruptedRender.openDocument(source);
    waitFor([&] { return ready; });
    const auto helperBin = directory.filePath("helpers");
    assert(QDir().mkpath(helperBin));
    QFile slow(helperBin + "/ddjvu");
    assert(slow.open(QIODevice::WriteOnly));
    slow.write("#!/bin/sh\nfor arg do last=$arg; done\nprintf partial > \"$last\"\nprintf '%s' \"$last\" > \"$YAGF_DJVU_MARKER\"\nexec /bin/sleep 10\n");
    slow.close();
    assert(slow.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    const QByteArray originalPath = qgetenv("PATH");
    const auto markerPath = directory.filePath("decode-marker");
    qputenv("YAGF_DJVU_MARKER", markerPath.toUtf8());
    qputenv("PATH", helperBin.toUtf8() + ':' + originalPath);
    interruptedRender.renderDocumentPage(1);
    waitFor([&] { return QFileInfo::exists(markerPath); });
    QFile marker(markerPath);
    assert(marker.open(QIODevice::ReadOnly));
    const QString partialPath = QString::fromUtf8(marker.readAll());
    interruptedRender.cancel();
    waitFor([&] { return stopped; });
    assert(!QFileInfo::exists(partialPath));
    qputenv("PATH", originalPath);
    qunsetenv("YAGF_DJVU_MARKER");
    interruptedRender.renderDocumentPage(1);
    waitFor([&] { return !retried.isNull(); });
    assert(retried.size() == sizes[0]);

    // Missing helper tools fail clearly, before any decode job starts.
    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", directory.filePath("empty-bin").toUtf8());
    error.clear();
    processor.openDocument(source);
    assert(error.contains("DjVuLibre") && !processor.isBusy());
    qputenv("PATH", oldPath);
    QFile unchanged(source);
    assert(unchanged.open(QIODevice::ReadOnly) && unchanged.readAll() == originalSource);
}
