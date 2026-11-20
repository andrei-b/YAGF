#include "bookvocabulary.h"
#include "correctionmemory.h"
#include "documentprocessor.h"
#include "symbolshape.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QPainter>
#include <QTemporaryDir>
#include <QTimer>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf;

static core::OcrPage page(int number, const QString &reading, double confidence, bool reviewed = false)
{
    core::OcrPage result;
    result.number = number;
    result.width = result.sourceWidth = 80;
    result.height = result.sourceHeight = 40;
    result.language = "eng";
    result.engineId = "tesseract";
    result.words.append({number, 1, 1, 1, QRect(2, 2, 12, 20), reading, reading, confidence, reviewed});
    result.rebuildHierarchy();
    return result;
}

static void script(const QString &path, const QByteArray &body)
{
    QFile file(path);
    assert(file.open(QIODevice::WriteOnly));
    assert(file.write(body) == body.size());
    file.close();
    assert(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
}

static void testSimilarSymbols()
{
    // Two scans of an L with a one-cell crop offset and a nick in its foot.
    auto shape = [](QPoint offset, bool nick, bool dot) {
        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter pen(&image);
        pen.translate(offset);
        pen.fillRect(3, 2, 3, 11, Qt::black);
        pen.fillRect(3, 10, 8, 3, Qt::black);
        if (nick) pen.fillRect(10, 12, 1, 1, Qt::white);
        if (dot) pen.fillRect(10, 2, 1, 1, Qt::black);
        pen.end();
        return processing::symbolShape(image, image.rect());
    };

    auto previous = page(1, "L", 98, true);
    previous.words[0].symbolShape = shape({}, false, false);
    auto current = page(4, "I", 60);
    current.words[0].symbolShape = shape({1, 1}, true, false);
    core::OcrDocument document;
    document.replacePage(previous);
    auto suggestions = [&](bool similar) {
        return core::CorrectionMemory(document, similar).suggestions(current, 0);
    };
    assert(suggestions(false).isEmpty());
    auto matches = suggestions(true);
    assert(matches.size() == 1 && matches.first().symbolMatch == core::SymbolMatch::Similar &&
           matches.first().replacement == "L" && matches.first().occurrences == 1);
    assert(current.words.first().text == "I" && !current.words.first().reviewed);

    current.words[0].symbolShape = shape({1, 1}, true, true);
    assert(suggestions(true).isEmpty()); // A distant dot changes the symbol.
    current.words[0].symbolShape = shape({1, 1}, true, false);
    previous.words[0].reviewed = false;
    previous.words.append(previous.words.first());
    document.replacePage(previous);
    assert(suggestions(true).isEmpty()); // Two occurrences on one page.
    auto second = previous;
    second.number = 2;
    second.words.resize(1);
    second.words[0].symbolShape = shape({}, true, false);
    document.replacePage(second);
    assert(suggestions(true).size() == 1 && suggestions(true).first().occurrences == 2);
    current.language = "deu";
    assert(suggestions(true).isEmpty());
    current.language = "eng";
    current.number = 2;
    assert(suggestions(true).isEmpty());
    current.number = 4;
    current.words[0].confidence = 85;
    assert(suggestions(true).isEmpty());
    current.words[0].confidence = 60;

    second.words[0].text = second.words[0].originalText = "J";
    document.replacePage(second);
    assert(suggestions(true).isEmpty()); // Competing labels among similar masks.
    second.words[0].reviewed = true;
    second.words[0].symbolShape = current.words.first().symbolShape;
    document.replacePage(second);
    matches = suggestions(true);
    assert(matches.size() == 1 && matches.first().symbolMatch == core::SymbolMatch::Exact &&
           matches.first().replacement == "J");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testSimilarSymbols();
    QImage scan(80, 40, QImage::Format_RGB32);
    scan.fill(Qt::white);
    QPainter painter(&scan);
    painter.fillRect(2, 2, 3, 20, Qt::black);
    painter.fillRect(2, 19, 12, 3, Qt::black);
    painter.end();
    auto confirmed = page(1, "1", 70, true);
    auto &example = confirmed.words[0];
    example.text = "L";
    example.symbolShape = processing::symbolShape(scan, example.bounds);
    assert(example.symbolShape.size() == 32);
    assert(processing::symbolShape(scan, QRect(30, 3, 10, 10)).isEmpty());
    const auto doubled = scan.scaled(160, 80, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    assert(processing::symbolShape(doubled, QRect(4, 4, 24, 40)) == example.symbolShape);

    core::OcrDocument book;
    book.sourcePath = "sample.pdf";
    book.totalPages = 5;
    book.replacePage(confirmed);
    auto test = page(3, "I", 65);
    test.words[0].symbolShape = example.symbolShape;
    book.replacePage(test);
    core::OcrDocument restored;
    QString error;
    assert(core::OcrDocument::fromJson(book.toJson(), &restored, &error));
    const auto proposals = core::CorrectionMemory(restored).suggestions(*restored.page(3), 0);
    assert(proposals.size() == 1 && proposals[0].symbolMatch == core::SymbolMatch::Exact && proposals[0].replacement == "L");
    assert(restored.page(3)->words[0].text == "I" && !restored.page(3)->words[0].reviewed);
    test.language = "deu";
    assert(core::CorrectionMemory(book).suggestions(test, 0).isEmpty());
    test.language = "eng";
    auto conflict = page(2, "J", 99, true);
    conflict.words[0].symbolShape = example.symbolShape;
    book.replacePage(conflict);
    assert(core::CorrectionMemory(book).suggestions(test, 0).isEmpty());
    conflict.words[0].text = conflict.words[0].originalText = "L";
    conflict.words[0].reviewed = false;
    book.removePage(1);
    book.replacePage(conflict);
    assert(core::CorrectionMemory(book).suggestions(test, 0).isEmpty());
    auto earlier = conflict;
    earlier.number = 1;
    book.replacePage(earlier);
    assert(core::CorrectionMemory(book).suggestions(test, 0).first().occurrences == 2);
    test.number = 1;
    assert(core::CorrectionMemory(book).suggestions(test, 0).isEmpty());

    core::BookVocabulary vocab;
    auto noisy = page(1, "QWldget", 99);
    noisy.words.append(noisy.words.first());
    vocab.addPage(noisy);
    assert(vocab.terms().isEmpty());
    noisy.number = 2;
    vocab.addPage(noisy);
    assert(vocab.terms() == QStringList{"QWldget"});
    noisy.words.resize(1);
    noisy.words[0].reviewed = true;
    noisy.words[0].text = "QWidget";
    vocab.addPage(noisy);
    assert(vocab.terms() == QStringList{"QWidget"});

    // On an actual batch entry point, capture the temporary dictionary while
    // the Tesseract subprocess can read it. Pages 2 and 4 produce new evidence.
    QTemporaryDir directory;
    assert(directory.isValid());
    // The vocabulary fixture exercises OCR hints, independently of math models.
    qputenv("XDG_DATA_HOME", QFile::encodeName(directory.path()));
    const QString imagePath = directory.filePath("scan.png");
    assert(scan.save(imagePath));
    qputenv("PATH", QFile::encodeName(directory.path()) + ':' + qgetenv("PATH"));
    qputenv("SCAN_IMAGE", QFile::encodeName(imagePath));
    qputenv("HINT_LOG", QFile::encodeName(directory.filePath("hints")));
    script(directory.filePath("pdfinfo"), "#!/bin/sh\necho 'Pages: 5'\n");
    script(directory.filePath("pdftoppm"), "#!/bin/sh\nfor arg do last=$arg; done\ncp \"$SCAN_IMAGE\" \"$last.png\"\n");
    script(directory.filePath("tesseract"), R"(#!/bin/sh
echo BEGIN >> "$HINT_LOG"
while [ "$#" -gt 0 ]; do
    if [ "$1" = "--user-words" ]; then cat "$2" >> "$HINT_LOG" || exit 1; break; fi
    shift
done
echo END >> "$HINT_LOG"
printf 'level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n'
printf '5\t1\t1\t1\t1\t1\t2\t2\t12\t20\t98\tL\n'
printf '5\t1\t1\t1\t1\t2\t25\t2\t30\t20\t97\tEigenmode\n'
)");

    book.pages.clear();
    auto corrected = page(1, "QWldget", 60, true);
    corrected.words[0].text = "QWidget";
    book.replacePage(corrected);
    auto foreign = page(3, "Deutsch", 99, true);
    foreign.language = "deu";
    book.replacePage(foreign);
    book.replacePage(page(5, "FutureTerm", 99, true));

    processing::DocumentProcessor processor(nullptr, &book);
    QEventLoop loop;
    int completed = 0;
    QObject::connect(&processor, &processing::DocumentProcessor::documentOpened, &loop, [&](int count) {
        assert(count == 5);
        processor.recognizePages({{2, "eng", {}}, {4, "eng", {}}, {5, "eng", {}}});
    });
    QObject::connect(&processor, &processing::DocumentProcessor::batchPageRecognized, &loop,
        [&](core::OcrPage recognized, const QSize &) {
            assert(recognized.words[0].symbolShape == example.symbolShape);
            assert(recognized.words[1].symbolShape.isEmpty());
            book.replacePage(std::move(recognized));
            ++completed;
        });
    QObject::connect(&processor, &processing::DocumentProcessor::batchFinished, &loop, &QEventLoop::quit);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    processor.openDocument(directory.filePath("book.pdf"));
    loop.exec();
    assert(completed == 3 && !processor.isBusy());
    QFile log(directory.filePath("hints"));
    assert(log.open(QIODevice::ReadOnly));
    assert(log.readAll() == "BEGIN\nQWidget\nEND\nBEGIN\nQWidget\nEND\nBEGIN\nQWidget\nEigenmode\nL\nEND\n");
}
