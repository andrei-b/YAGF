#include "scientificbookexporter.h"
#include "projectfile.h"

#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    core::OcrDocument book;
    book.sourcePath = dir.filePath("scan.png");
    QImage image(120, 150, QImage::Format_RGB32);
    image.fill(Qt::white);
    image.setPixelColor(10, 20, Qt::blue);
    assert(image.save(book.sourcePath));
    core::OcrPage page;
    page.width = page.sourceWidth = image.width();
    page.height = page.sourceHeight = image.height();
    const QStringList text{"[Picture]", "Fig. 1: A < B", "Model", R"(\[x_1 = \frac{1}{2}\])",
                           "Quantity\nA B\t42"};
    const QStringList types{"picture", "caption", "sectionheader", "equation", "table"};
    for (int i = 0; i < text.size(); ++i) {
        page.words.append({1, i + 1, 1, 1, QRect(10, 20 * (i + 1), 50, 20), text[i], text[i], 90});
        page.words.last().layoutBlock = true;
    }
    page.rebuildHierarchy();
    for (int i = 0; i < types.size(); ++i) page.regions[i].type = types[i];
    const QJsonObject heading{{"header", true}, {"colspan", 2}, {"rowspan", 1}};
    const QJsonObject cell{{"header", false}, {"colspan", 1}, {"rowspan", 1}};
    page.regions.last().tableLayout = QJsonArray{QJsonArray{heading}, QJsonArray{cell, cell}};
    // The printed heading belongs before the figure, irrespective of its y coordinate.
    assert(page.moveRegion(2, -1) && page.moveRegion(1, -1));
    page.words.last().text.replace("42", "43");
    page.words.last().reviewed = true;
    book.replacePage(page);
    QString error;
    const auto project = dir.filePath("book.yagf.json");
    assert(core::ProjectFile::save(project, book, &error));
    core::OcrDocument restored;
    assert(core::ProjectFile::load(project, &restored, &error));
    assert(restored.pages.first().words.last().layoutBlock);
    const auto path = dir.filePath("book.html");
    std::atomic_bool cancel{false};
    using processing::ExportResult;
    using processing::ScientificBookExporter;
    assert(ScientificBookExporter::write(restored, path, cancel).status == ExportResult::Status::Success);
    QFile file(path);
    assert(file.open(QIODevice::ReadOnly));
    const auto html = file.readAll();
    file.close();
    assert(html.contains("<h2 class=\"sectionheader\">Model</h2>"));
    assert(html.indexOf("<h2") < html.indexOf("<figure>"));
    assert(html.contains("<figcaption>Fig. 1: A &lt; B</figcaption></figure>"));
    assert(html.contains(R"(\[x_1 = \frac{1}{2}\])"));
    assert(html.contains("<th colspan=\"2\">Quantity</th>"));
    assert(html.contains("<td>A B</td><td>43</td>"));
    const auto prefix = QByteArrayLiteral("data:image/png;base64,");
    const auto start = html.indexOf(prefix) + prefix.size();
    const QImage crop = QImage::fromData(QByteArray::fromBase64(html.mid(start, html.indexOf('"', start) - start)));
    assert(crop.size() == QSize(50, 20) && crop.pixelColor(0, 0) == Qt::blue);
    cancel = true;
    assert(ScientificBookExporter::write(restored, path, cancel).status == ExportResult::Status::Cancelled);
    cancel = false;
    restored.sourcePath = dir.filePath("missing.png");
    assert(ScientificBookExporter::write(restored, path, cancel).status == ExportResult::Status::Error);
    assert(file.open(QIODevice::ReadOnly) && file.readAll() == html);
}
