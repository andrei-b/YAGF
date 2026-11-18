#include "documentprocessor.h"
#include "tesseractengine.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
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
    std::expected<yagf::core::OcrPage, OcrError> recognize(
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
        return page;
    }
    bool waitForCancel;
    bool invalid;
};
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
    const QByteArray body = "#!/bin/sh\ncat \"$TEST_TSV_FILE\"\n";
    assert(script.write(body) == body.size());
    script.close();
    assert(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    qputenv("PATH", QFile::encodeName(directory.path()) + ':' + qgetenv("PATH"));
    qputenv("TEST_TSV_FILE", QFile::encodeName(fixturePath));
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    TesseractEngine engine;
    assert(engine.capabilities().supportsConfidence);
    auto recognized = engine.recognize(image, {.language = QStringLiteral("eng")}, {});
    assert(recognized && recognized->words.size() == 2);
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
    assert(failure.contains(QStringLiteral("invalid word geometry")));
    assert(!processor.isBusy());
}
