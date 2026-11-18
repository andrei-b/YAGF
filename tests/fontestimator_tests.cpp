#include "scientificbookexporter.h"
#include "fontestimator.h"
#include "documentprocessor.h"
#include "projectfile.h"
#include "wordeditor.h"
#include "pdfexporter.h"

#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf;

struct Fixture {
    QImage image;
    core::OcrPage page;
};

static Fixture fixture(const QString &family, bool bold, bool italic, int pixels = 40, bool passage = false)
{
    const QString text = QStringLiteral("Typography preserves readable fonts");
    QFont font(family);
    font.setPixelSize(pixels);
    font.setBold(bold);
    font.setItalic(italic);
    QFontMetrics metrics(font);
    const int height = metrics.height() + 16;
    Fixture result;
    result.image = QImage(metrics.horizontalAdvance(text) + 32, passage ? height * 2 : height, QImage::Format_RGB32);
    result.image.setDotsPerMeterX(5906);
    result.image.setDotsPerMeterY(5906);
    result.image.fill(Qt::white);
    QPainter painter(&result.image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(Qt::black);
    painter.drawText(QPoint(12, 8 + metrics.ascent()), text);
    if (passage) painter.drawText(QPoint(12, height + 8 + metrics.ascent()), text);
    painter.end();
    result.page.width = result.page.sourceWidth = result.image.width();
    result.page.height = result.page.sourceHeight = result.image.height();
    core::OcrWord word;
    word.block = word.paragraph = word.line = 1;
    word.bounds = result.image.rect();
    word.originalText = word.text = passage ? text + '\n' + text : text;
    word.layoutBlock = passage;
    result.page.words.append(word);
    result.page.rebuildHierarchy();
    return result;
}

class FixtureEngine : public processing::IOcrEngine {
public:
    explicit FixtureEngine(core::OcrPage page) : page(std::move(page)) {}
    QString id() const override { return QStringLiteral("fixture-remote"); }
    processing::EngineCapabilities capabilities() const override { return {.supportsRegions = true}; }
    std::expected<core::OcrPage, processing::OcrError> recognize(
        const QImage &, const processing::OcrOptions &options, std::stop_token) override
    {
        assert(options.preserveFormatting == enabled);
        return page;
    }
    bool enabled = false;
    core::OcrPage page;
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const auto installed = QFontDatabase::families();
    for (const auto &family : {QStringLiteral("Liberation Serif"), QStringLiteral("Liberation Sans"), QStringLiteral("Liberation Mono")}) {
        if (!installed.contains(family)) { std::puts("Skipping font fixtures: Liberation fonts unavailable."); return 77; }
        for (bool bold : {false, true}) for (bool italic : {false, true}) {
            auto sample = fixture(family, bold, italic);
            const auto original = sample.page.words.first();
            processing::estimateTextFormatting(sample.page, sample.image);
            const auto &word = sample.page.words.first();
            std::printf("%s %d/%d -> %s %d/%d %dpt\n", qPrintable(family), bold, italic,
                        qPrintable(word.fontFamily), word.bold, word.italic, word.fontSizePt);
            assert(word.fontFamily == family && word.bold == bold && word.italic == italic);
            assert(std::abs(word.fontSizePt - 19) <= 1);
            assert(word.text == original.text && word.bounds == original.bounds && !word.reviewed);
        }
    }
    auto sample = fixture(QStringLiteral("Liberation Serif"), true, false, 48, true);
    processing::estimateTextFormatting(sample.page, sample.image);
    assert(sample.page.words.first().fontFamily == "Liberation Serif" && sample.page.words.first().bold);
    auto cancelled = fixture(QStringLiteral("Liberation Sans"), false, false);
    std::stop_source stop;
    stop.request_stop();
    processing::estimateTextFormatting(cancelled.page, cancelled.image, stop.get_token());
    assert(cancelled.page.words.first().fontFamily.isEmpty());
    cancelled.page.words.first().reviewed = true;
    cancelled.page.words.first().fontFamily = "Manual face";
    processing::estimateTextFormatting(cancelled.page, cancelled.image);
    assert(cancelled.page.words.first().fontFamily == "Manual face");

    QTemporaryDir directory;
    assert(directory.isValid());
    const QString source = directory.filePath("source.png");
    assert(sample.image.save(source));
    core::OcrDocument book;
    book.sourcePath = source;
    book.preserveFormatting = true;
    book.replacePage(sample.page);
    QString error;
    const QString project = directory.filePath("project.yagf.json");
    assert(core::ProjectFile::save(project, book, &error));
    core::OcrDocument restored;
    assert(core::ProjectFile::load(project, &restored, &error));
    assert(restored.preserveFormatting && restored.pages.first().words.first().fontFamily == "Liberation Serif");
    assert(restored.pages.first().words.first().sourceFontSizePx > 0);
    assert(restored.layoutHtml().contains("font-family:") && restored.layoutHtml().contains("Liberation Serif"));
    auto edited = core::WordEditor::apply(restored.pages.first(), {0}, {{"Corrected", true, false, 23, "Liberation Mono"}});
    assert(edited && edited->words.first().fontFamily == "Liberation Mono" && edited->words.first().bold);
    const auto malformed = [&] {
        auto json = book.toJson();
        json.insert("preserve_formatting", "true");
        return json;
    }();
    assert(!core::OcrDocument::fromJson(malformed, &restored, &error));
    auto legacy = book.toJson();
    legacy.remove("preserve_formatting");
    assert(core::OcrDocument::fromJson(legacy, &restored, &error) && !restored.preserveFormatting);
    std::atomic_bool cancel{false};
    const QString htmlPath = directory.filePath("book.html");
    assert(processing::ScientificBookExporter::write(book, htmlPath, cancel).status == processing::ExportResult::Status::Success);
    QFile html(htmlPath);
    assert(html.open(QIODevice::ReadOnly));
    const auto output = html.readAll();
    assert(output.contains("Liberation Serif") && output.contains("font-weight:bold"));
    assert(processing::PdfExporter::write(book, directory.filePath("formatted.pdf"), false, cancel).status ==
           processing::ExportResult::Status::Success);

    // Processor integration exercises the option with a backend returning paragraph blocks.
    auto plain = fixture(QStringLiteral("Liberation Serif"), true, false, 48, true);
    auto engine = std::make_unique<FixtureEngine>(plain.page);
    auto *fixtureEngine = engine.get();
    processing::DocumentProcessor processor(nullptr, &book);
    processor.setEngine(std::move(engine));
    for (bool enabled : {false, true}) {
        book.preserveFormatting = enabled;
        fixtureEngine->enabled = enabled;
        QEventLoop loop;
        bool received = false;
        const auto connection = QObject::connect(&processor, &processing::DocumentProcessor::recognized, &loop,
            [&](core::OcrPage page, const QPoint &, const QRect &) {
                received = true;
                assert(page.words.first().fontFamily.isEmpty() != enabled);
                if (enabled) assert(page.words.first().bold);
                loop.quit();
            });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        processor.recognize(source, "eng", 1);
        loop.exec();
        assert(received);
        QObject::disconnect(connection);
    }
}
