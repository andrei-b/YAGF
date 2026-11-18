#include "documentprocessor.h"
#include "specialistocr.h"
#include "wordeditor.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf;
using processing::IOcrEngine;

namespace {
class FixtureEngine final : public IOcrEngine {
public:
    FixtureEngine(QString name, core::OcrPage output) : name(std::move(name)), output(std::move(output)) {}
    [[nodiscard]] QString id() const override { return name; }
    [[nodiscard]] processing::EngineCapabilities capabilities() const override { return {.supportsRegions = true}; }
    std::expected<core::OcrPage, processing::OcrError> recognize(const QImage &image,
        const processing::OcrOptions &options, std::stop_token) override
    {
        images.append(image.size());
        if (fail) return std::unexpected(processing::OcrError{"Offline"});
        auto page = output;
        page.width = page.sourceWidth = image.width();
        page.height = page.sourceHeight = image.height();
        page.number = options.pageNumber;
        return page;
    }
    QString name;
    core::OcrPage output;
    QVector<QSize> images;
    bool fail = false;
};

void add(core::OcrPage &page, int line, int x, const QString &text, double confidence)
{
    page.words.append({1, 1, 1, line, QRect(x, 12 * line, 10, 8), text, text, confidence});
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    QImage image(120, 90, QImage::Format_RGB32);
    image.fill(Qt::white);
    const QString path = dir.filePath("page.png");
    assert(image.save(path));

    core::OcrPage primary, eq, phrase;
    add(primary, 1, 6, "Heading", 98);
    add(primary, 2, 6, "x", 93);
    add(primary, 2, 20, "=2", 40);
    add(primary, 3, 6, "the", 99);
    add(primary, 3, 20, "rnean", 40);
    add(primary, 4, 6, "garble", 95); // An explicitly labelled equation with no OCR math operator.
    add(eq, 1, 1, "x^{2}", 90);
    add(phrase, 1, 1, "the mean", 90);
    core::OcrDocument book;
    book.sourcePath = path;
    core::OcrPage old;
    old.width = old.sourceWidth = image.width();
    old.height = old.sourceHeight = image.height();
    old.words = primary.words;
    book.replacePage(old);
    book.page(1)->regions[0].bounds = QRect(4, 47, 13, 10);
    book.page(1)->regions[0].type = "equation";

    processing::DocumentProcessor processor(nullptr, &book);
    processor.setEngine(std::make_unique<FixtureEngine>("tesseract", primary));
    auto math = std::make_unique<FixtureEngine>("surya-remote", eq);
    auto prose = std::make_unique<FixtureEngine>("doctr-remote", phrase);
    auto *mathFixture = math.get();
    auto *proseFixture = prose.get();
    processor.setSpecialists(std::move(math), std::move(prose));
    QEventLoop loop;
    core::OcrPage page;
    QObject::connect(&processor, &processing::DocumentProcessor::recognized, &loop,
        [&](core::OcrPage result, const QPoint &, const QRect &) { page = std::move(result); loop.quit(); });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    processor.recognize(path, "eng", 1);
    loop.exec();
    assert(page.engineId == "tesseract" && page.words.size() == 6);
    assert((mathFixture->images == QVector<QSize>{{28,12}, {14,12}}));
    assert((proseFixture->images == QVector<QSize>{{28,12}}));
    assert(page.words[0].alternative.text.isEmpty() && page.words[2].text == "=2");
    assert((page.specialistSpan(1) == QVector<int>{1, 2}));
    assert(page.words[1].alternative.formula && !page.words[3].alternative.formula);
    book.replacePage(page);
    core::OcrDocument reopened;
    QString error;
    assert(core::OcrDocument::fromJson(book.toJson(), &reopened, &error));
    auto accepted = core::WordEditor::acceptSpecialist(*reopened.page(1), 1, "x^{2}");
    assert(accepted && accepted->words.size() == 5);
    const auto &formula = accepted->words[1];
    assert(formula.reviewed && formula.layoutBlock && formula.readingEngine == "surya-remote");
    assert(formula.originalText == "x =2" && formula.sourceBoxes.size() == 2);
    assert(accepted->words[0].id == page.words[0].id && accepted->words[2].id == page.words[3].id);
    accepted = core::WordEditor::acceptSpecialist(*accepted, 2, "the mean");
    assert(accepted && accepted->words[3].text == "mean" && accepted->words[3].originalText == "rnean");
    assert(accepted->words[3].readingEngine == "doctr-remote");
    book.replacePage(*accepted);
    assert(core::OcrDocument::fromJson(book.toJson(), &reopened, &error));
    assert(reopened.page(1)->words[1].readingEngine == "surya-remote");
    page.words[2].reviewed = true;
    assert(page.specialistSpan(1).isEmpty() && !core::WordEditor::acceptSpecialist(page, 1, "stale"));
    primary = page;
    for (auto &word : primary.words) word.alternative = {};
    mathFixture->fail = true;
    auto warnings = processing::collectSpecialistReadings(primary, image, {QRect(4, 47, 13, 10)},
        mathFixture, proseFixture, {});
    assert(warnings.size() == 1 && warnings.first().contains("Offline"));
    assert(primary.words[5].alternative.text.isEmpty() && primary.words[3].alternative.text == "the mean");
    std::stop_source stop;
    stop.request_stop();
    const auto calls = proseFixture->images.size();
    processing::collectSpecialistReadings(primary, image, {}, mathFixture, proseFixture, stop.get_token());
    assert(proseFixture->images.size() == calls);
}
