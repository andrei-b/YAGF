#include "documentprocessor.h"
#include "tesseractengine.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QTimer>

#include <chrono>
#include <thread>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf::processing;

namespace {
class FakeEngine final : public IOcrEngine {
public:
    explicit FakeEngine(bool waitForCancel, bool invalid = false)
        : waitForCancel(waitForCancel), invalid(invalid) {}
    [[nodiscard]] QString id() const override { return QStringLiteral("fake"); }
    [[nodiscard]] EngineCapabilities capabilities() const override
    { return {.supportsRegions = true, .supportsWordBoxes = true}; }
    std::expected<yagf::core::RecognitionResult, OcrError> recognizeResult(
        const QImage &image, const OcrOptions &options, std::stop_token stop) override
    {
        if (waitForCancel) {
            for (int i = 0; i < 200 && !stop.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return std::unexpected(OcrError{{}, true});
        }
        yagf::core::OcrPage page;
        page.number = options.pageNumber;
        page.width = page.sourceWidth = image.width();
        page.height = page.sourceHeight = image.height();
        page.engineId = id();
        page.language = options.language;
        page.words.append({options.pageNumber, 1, 1, 1,
                           invalid ? QRect(image.width() - 1, 1, 10, 10) : QRect(1, 1, 10, 10),
                           QStringLiteral("source"), QStringLiteral("source"), 90});
        page.rebuildHierarchy();
        return yagf::processing::recognitionFromPage(page);
    }
    bool waitForCancel;
    bool invalid;
};

void testInlineScriptEvidence(const QImage &image)
{
    QTemporaryDir bin;
    QFile script(bin.filePath("tesseract"));
    assert(script.open(QIODevice::WriteOnly));
    script.write(R"SH(#!/bin/sh
if [ "$1" = --print-parameters ]; then
    printf 'Tesseract parameters:\nlstm_reread_subscripts\t0\tReread subscripts\n'
    exit 0
fi
printf 'level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n5\t1\t1\t1\t1\t1\t10\t20\t38\t12\t90\tA12\n' > "$2.tsv"
cat > "$2.hocr" <<'XML'
<span class="ocrx_word" title="bbox 10 20 48 32; x_wconf 90">
  <span class="ocrx_cinfo" title="x_bboxes 10 20 30 30">A</span>
  <span class="ocrx_cinfo" title="x_bboxes 31 25 40 32"><sub>1</sub></span>
  <span class="ocrx_cinfo" title="x_bboxes 40 20 48 27"><sup>2</sup></span>
</span>
XML
if [ -e "$0.invalid" ]; then echo '<' >> "$2.hocr"; fi
)SH");
    script.close();
    assert(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const auto path = qgetenv("PATH");
    qputenv("PATH", QFile::encodeName(bin.path()) + ':' + path);
    TesseractEngine engine;
    auto page = engine.recognize(image, {.language = "eng"}, {});
    assert(page && page->words.size() == 1);
    const auto &word = page->words.first();
    assert(word.text == "A12" && word.scripts.size() == 2);
    const auto &glyph = word.scripts.first();
    assert(glyph.start == 1 && glyph.length == 1 && glyph.imageBox(word.bounds) == QRectF(31,25,9,7) &&
           glyph.position == yagf::core::ScriptPosition::Subscript);
    const auto &raised = word.scripts.last();
    assert(raised.start == 2 && raised.length == 1 && raised.imageBox(word.bounds) == QRectF(40,20,8,7) &&
           raised.position == yagf::core::ScriptPosition::Superscript);
    const auto validEvidence = engine.recognizeResult(image, {.language = "eng"}, {});
    assert(validEvidence && validEvidence->structuredOutput["inline_subscripts"].toBool());
    assert(!validEvidence->structuredOutput.contains("warnings"));
    const auto json = recognitionResultJson(*validEvidence);
    const auto restored = yagf::core::readRecognitionResult(json);
    assert(restored && restored->text.first().scripts.size() == 2);
    assert(restored->text.first().scripts.last().position == yagf::core::ScriptPosition::Superscript);
    QFile invalid(script.fileName() + ".invalid");
    assert(invalid.open(QIODevice::WriteOnly));
    invalid.close();
    page = engine.recognize(image, {.language = "eng"}, {});
    assert(page && page->words.first().text == "A12" && page->words.first().scripts.isEmpty());
    const auto invalidEvidence = engine.recognizeResult(image, {.language = "eng"}, {});
    assert(invalidEvidence && invalidEvidence->structuredOutput["inline_subscripts"].toBool());
    assert(invalidEvidence->structuredOutput["warnings"].toArray().size() == 1);
    qputenv("PATH", path);
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QByteArray tsv =
        "level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n"
        "5\t1\t1\t1\t1\t1\t10\t20\t30\t12\t91.2\tHello\n"
        "5\t1\t1\t1\t1\t2\t50\t20\t40\t12\t75\tworld\n"
        "5\t1\t1\t1\t1\t3\t190\t20\t30\t12\t80\tOutside\n";
    auto parsed = TesseractEngine::parseTsv(tsv, QSize(200, 100),
                                            {.language = QStringLiteral("eng"), .pageNumber = 1});
    assert(parsed && parsed->words.size() == 2);
    assert(parsed->words.at(0).bounds == QRect(10, 20, 30, 12));
    assert(parsed->engineId == QStringLiteral("tesseract"));
    assert(!TesseractEngine::parseTsv("broken", QSize(200, 100),
                                     {.language = QStringLiteral("eng")}));
    const auto withImage = TesseractEngine::parseTsv(tsv +
        "5\t1\t2\t1\t1\t1\t80\t50\t60\t40\t95\t \n", QSize(200, 100), {.language = "rus"});
    assert(withImage && withImage->words.size() == 2 && withImage->imageRegions.size() == 1);
    assert(withImage->imageRegions[0].bounds == QRect(80, 50, 60, 40));
    assert(withImage->imageRegions[0].autoDetected && withImage->imageRegions[0].content.isEmpty());

    QTemporaryDir directory;
    assert(directory.isValid());
    const QString scriptPath = directory.filePath(QStringLiteral("tesseract"));
    const QString fixturePath = directory.filePath(QStringLiteral("result.tsv"));
    QFile fixture(fixturePath);
    assert(fixture.open(QIODevice::WriteOnly));
    assert(fixture.write(tsv) == tsv.size());
    fixture.close();
    QFile script(scriptPath);
    assert(script.open(QIODevice::WriteOnly));
    const QByteArray body = "#!/bin/sh\n[ \"$3\" = -l ] && [ \"$4\" = \"$TEST_MODEL_LANGUAGE\" ] || exit 1\ncat \"$TEST_TSV_FILE\"\n";
    assert(script.write(body) == body.size());
    script.close();
    assert(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    qputenv("PATH", QFile::encodeName(directory.path()) + ':' + qgetenv("PATH"));
    qputenv("TEST_TSV_FILE", QFile::encodeName(fixturePath));
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    testInlineScriptEvidence(image);
    TesseractEngine engine;
    assert(engine.capabilities().supportsConfidence);
    qputenv("TEST_MODEL_LANGUAGE", "eng");
    auto recognized = engine.recognize(image, {.language = QStringLiteral("eng")}, {});
    assert(recognized && recognized->words.size() == 2);
    const auto evidence = engine.recognizeResult(image, {.language = QStringLiteral("eng")}, {});
    assert(evidence && evidence->text.size() == 2);
    assert(evidence->text[0].granularity == yagf::core::TextGranularity::Word);
    assert(evidence->text[0].bounds == QRect(10,20,30,12));
    for (const QString &language : {QString{}, QStringLiteral("auto"), QStringLiteral("en")}) {
        auto english = engine.recognize(image, {.language = language}, {});
        assert(english && english->language == "eng");
    }
    qputenv("TEST_MODEL_LANGUAGE", "deu+eng");
    auto mixed = engine.recognize(image, {.language = QStringLiteral("de+en+eng")}, {});
    assert(mixed && mixed->language == "deu+eng");
    std::stop_source stopped;
    stopped.request_stop();
    auto cancelled = engine.recognize(image, {.language = QStringLiteral("eng")}, stopped.get_token());
    assert(!cancelled && cancelled.error().cancelled);
    // PDF layout recovery uses an atomic cancellation flag while Tesseract runs.
    assert(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
    script.write("#!/bin/sh\nexec sleep 5\n"); script.close();
    std::atomic_bool exportCancelled{false};
    std::jthread interruptExport([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        exportCancelled = true;
    });
    QElapsedTimer elapsed;
    elapsed.start();
    auto cancelledExport = engine.recognize(image, {.language = QStringLiteral("eng")}, {}, &exportCancelled);
    assert(!cancelledExport && cancelledExport.error().cancelled && elapsed.elapsed() < 2000);

    const QString imagePath = directory.filePath(QStringLiteral("source.png"));
    assert(image.save(imagePath));
    DocumentProcessor processor;
    processor.setEngine(std::make_unique<FakeEngine>(false));
    assert(processor.engineId() == QStringLiteral("fake"));
    QEventLoop loop;
    bool received = false;
    QObject::connect(&processor, &DocumentProcessor::recognized, &loop,
        [&](const yagf::core::OcrPage &page, const QPoint &, const QRect &) {
            assert(page.engineId == QStringLiteral("fake"));
            assert(page.width == 200 && page.words.size() == 1);
            received = true;
            loop.quit();
        });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    processor.recognize(imagePath, QStringLiteral("eng"), 1);
    assert(processor.isBusy());
    loop.exec();
    assert(received && !processor.isBusy());

    processor.setEngine(std::make_unique<FakeEngine>(true));
    bool wasCancelled = false;
    QObject::connect(&processor, &DocumentProcessor::cancelled, &loop, [&] {
        wasCancelled = true;
        loop.quit();
    });
    processor.recognize(imagePath, QStringLiteral("eng"), 1);
    QTimer::singleShot(30, &processor, &DocumentProcessor::cancel);
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();
    assert(wasCancelled && !processor.isBusy());

    processor.setEngine(std::make_unique<FakeEngine>(false, true));
    QString failure;
    QObject::connect(&processor, &DocumentProcessor::failed, &loop, [&](const QString &message) {
        failure = message;
        loop.quit();
    });
    processor.recognize(imagePath, QStringLiteral("eng"), 1);
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();
    assert(failure.contains(QStringLiteral("Invalid recognition evidence")));
    assert(!processor.isBusy());

    // Corrected layout remains the default; raw mode skips generated outlines.
    assert(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
    assert(script.write(body) == body.size());
    script.close();
    qputenv("TEST_MODEL_LANGUAGE", "eng");
    QPainter painter(&image);
    painter.fillRect(10, 20, 30, 12, Qt::black);
    painter.fillRect(50, 20, 40, 12, Qt::black);
    painter.end();
    assert(image.save(imagePath));
    yagf::core::OcrDocument rawBook;
    rawBook.sourcePath = imagePath;
    assert(!rawBook.useRawOcrOutput);
    DocumentProcessor rawProcessor(nullptr, &rawBook);
    for (bool raw : {false, true}) {
        rawBook.useRawOcrOutput = raw;
        bool completed = false;
        QEventLoop rawLoop;
        const auto connection = QObject::connect(&rawProcessor, &DocumentProcessor::recognized, &rawLoop,
            [&](const yagf::core::OcrPage &page, const QPoint &, const QRect &) {
                assert(page.engineId == "tesseract" && page.words.size() == 2);
                assert(page.words[0].text == "Hello" && page.words[1].text == "world");
                assert(page.imageRegions.isEmpty() == raw);
                completed = true;
                rawLoop.quit();
            });
        QTimer::singleShot(3000, &rawLoop, &QEventLoop::quit);
        rawProcessor.recognize(imagePath, "eng", 1);
        rawLoop.exec();
        assert(completed);
        QObject::disconnect(connection);
    }

    // Raw OCR of several selected blocks keeps each crop's source geometry.
    QEventLoop selectedLoop;
    int selectedResults = 0;
    const auto selectedConnection = QObject::connect(&rawProcessor, &DocumentProcessor::recognized, &selectedLoop,
        [&](const yagf::core::OcrPage &page, const QPoint &offset, const QRect &region) {
            assert(page.words.size() == 2 && region.width() == 100);
            assert(offset == region.topLeft());
            assert(page.words[0].bounds == QRect(10, 20, 30, 12));
            assert(page.words[0].sourceBoxes == QVector<QRect>{page.words[0].bounds});
            if (++selectedResults == 2) selectedLoop.quit();
        });
    QTimer::singleShot(3000, &selectedLoop, &QEventLoop::quit);
    rawProcessor.recognizeTypedRegions(imagePath, "eng", 1,
        {{"left", {0, 0, 100, 100}, "eng", false, "text"},
         {"right", {100, 0, 100, 100}, "eng", false, "math"}});
    selectedLoop.exec();
    assert(selectedResults == 2);
    QObject::disconnect(selectedConnection);

    // Explicit non-text exclusions still apply when automatic layout is skipped.
    yagf::core::OcrPage excludedPage;
    excludedPage.width = excludedPage.sourceWidth = image.width();
    excludedPage.height = excludedPage.sourceHeight = image.height();
    excludedPage.imageRegions = {{"excluded", {10, 20, 30, 12}, {}, true}};
    rawBook.replacePage(excludedPage);
    QEventLoop excludedLoop;
    bool excluded = false;
    QObject::connect(&rawProcessor, &DocumentProcessor::recognized, &excludedLoop,
        [&](const yagf::core::OcrPage &page, const QPoint &, const QRect &) {
            assert(page.words.size() == 1 && page.words[0].text == "world");
            excluded = true;
            excludedLoop.quit();
        });
    QTimer::singleShot(3000, &excludedLoop, &QEventLoop::quit);
    rawProcessor.recognize(imagePath, "eng", 1);
    excludedLoop.exec();
    assert(excluded);
}
