#include "detail/pdfpagelayout.h"
#include "detail/pdfpagepainter.h"

#include <QGuiApplication>
#include <QPainter>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf;
using namespace yagf::processing::detail;

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QImage scan(800, 300, QImage::Format_RGB32);
    scan.fill(Qt::white);
    const QRect photograph(650, 200, 100, 60);
    QPainter source(&scan);
    source.fillRect(photograph, Qt::blue);
    source.end();
    core::OcrDocument document;
    document.sourcePath = "unchanged-source.png";
    core::OcrPage page;
    page.width = page.sourceWidth = scan.width();
    page.height = page.sourceHeight = scan.height();
    page.language = "auto";
    const QStringList lines{"First source line remains", "Second source line remains", "Third source line remains"};
    core::OcrWord block;
    block.block = block.paragraph = block.line = 1;
    block.bounds = scan.rect();
    block.layoutBlock = true;
    block.originalText = block.text = lines.join('\n');
    page.words.append(block);
    page.imageRegions.append({"photograph", photograph, {}, true});
    page.rebuildHierarchy();
    document.replacePage(page);
    const auto snapshot = document.toJson();

    int calls = 0;
    const LayoutRecognizer recognize = [&](const QImage &input, const QString &language, int number) {
        ++calls;
        assert(input.size() == scan.size() * 2 && language == "eng" && number == 1);
        core::OcrPage geometry;
        for (int row = 0; row < lines.size(); ++row) {
            const auto tokens = lines[row].split(' ');
            for (int column = 0; column < tokens.size(); ++column) {
                core::OcrWord word;
                word.block = word.paragraph = 1;
                word.line = row + 1;
                word.bounds = QRect(80 + column * 180, 40 + row * 100, 140, 32);
                word.originalText = word.text = tokens[column];
                geometry.words.append(word);
            }
        }
        return std::optional<core::OcrPage>(geometry);
    };
    std::atomic_bool cancel{false};
    const auto layout = preparePdfPage(page, scan, cancel, recognize);
    assert(calls == 1 && layout.page.words.size() == lines.size());
    for (int i = 0; i < lines.size(); ++i) {
        assert(layout.page.words[i].text == lines[i]);
        assert(layout.page.words[i].bounds.top() == 20 + i * 50);
    }
    // Erasure instructions belong to the export plan, never to model regions.
    assert(layout.textAreas.size() > layout.page.regions.size());
    for (const auto &region : layout.page.regions) assert(!region.id.isEmpty() && !region.paragraphs.isEmpty());
    assert(layout.graphics.contains(photograph));
    assert(document.toJson() == snapshot);

    QImage rendered = scan;
    QPainter painter(&rendered);
    assert(drawFormattedPage(painter, layout, scan, cancel));
    painter.end();
    assert(rendered.pixelColor(700, 230) == Qt::blue);
    assert(rendered != scan && document.toJson() == snapshot);

    // Provider absence and failure both fall back to the saved block.
    const auto unavailable = preparePdfPage(page, scan, cancel);
    assert(unavailable.page.words.size() == 1 && unavailable.page.words.first().text == block.text);
    const auto failed = preparePdfPage(page, scan, cancel,
        [](const QImage &, const QString &, int) { return std::nullopt; });
    assert(failed.page.words.size() == 1 && failed.page.words.first().bounds == block.bounds);

    cancel = true;
    const int beforeCancel = calls;
    const auto cancelled = preparePdfPage(page, scan, cancel, recognize);
    assert(calls == beforeCancel);
    QImage untouched = scan;
    QPainter stopped(&untouched);
    assert(!drawFormattedPage(stopped, cancelled, scan, cancel));
    stopped.end();
    assert(untouched == scan);
}
