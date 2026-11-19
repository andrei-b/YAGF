#include "verifiedsampleexporter.h"
#include "pdfexporter.h"
#include "exportpage.h"

#include <QGuiApplication>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QStandardPaths>
#include <QSet>
#include <QTemporaryDir>
#include <QXmlStreamReader>

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

static void formattedPdfChecks(const QTemporaryDir &directory)
{
    const QString source = directory.filePath("formatted-source.png");
    QImage scan(600, 500, QImage::Format_RGB32);
    scan.fill(QColor(250, 246, 232));
    const QRect photograph(460, 30, 90, 60);
    const QRect table(325, 370, 240, 85);
    QPainter sourcePainter(&scan);
    sourcePainter.setRenderHint(QPainter::TextAntialiasing);
    sourcePainter.fillRect(photograph, Qt::blue);
    sourcePainter.setPen(Qt::darkGreen);
    sourcePainter.drawRect(table.adjusted(1, 1, -1, -1));
    OcrDocument book;
    book.sourcePath = source;
    yagf::core::OcrPage page;
    page.width = page.sourceWidth = scan.width();
    page.height = page.sourceHeight = scan.height();
    const auto add = [&](QRect box, QString original, QString corrected, QString family, int pixels,
                         bool bold = false, bool italic = false) {
        QFont font(family);
        font.setPixelSize(pixels); font.setBold(bold); font.setItalic(italic);
        sourcePainter.setFont(font);
        sourcePainter.setPen(QColor(180, 20, 20));
        QFontMetricsF metrics(font);
        qreal y = box.top();
        for (const auto &line : original.split('\n')) {
            const auto ink = metrics.tightBoundingRect(line);
            sourcePainter.drawText(QPointF(box.left() - ink.left(), y - ink.top()), line);
            y += metrics.height();
        }
        yagf::core::OcrWord word;
        word.block = page.words.size() + 1;
        word.paragraph = word.line = 1;
        word.bounds = box;
        word.originalText = original; word.text = corrected;
        word.fontFamily = family; word.sourceFontSizePx = pixels;
        word.fontSizePt = word.sourceFontSizePt = qRound(pixels * 72.0 / 150);
        word.bold = bold; word.italic = italic;
        word.layoutBlock = original.contains('\n');
        page.words.append(word);
    };
    add(QRect(35, 30, 350, 55), "OriginalHeading", "CorrectedHeading", "Liberation Serif", 32, true);
    add(QRect(35, 120, 240, 200), "LeftOriginal\nLeftBeta\nLeftGamma", "LeftAlpha\nLeftBeta\nLeftGamma", "Liberation Serif", 24);
    add(QRect(325, 120, 240, 200), "RightAlpha\nRightBeta", "RightAlpha\nRightBeta", "Liberation Mono", 24, true, true);
    add(QRect(35, 380, 240, 40), "InferredText", "InferredText", "Liberation Sans", 24);
    page.words.last().fontFamily.clear();
    page.words.last().sourceFontSizePx = 0;
    sourcePainter.end();
    assert(scan.save(source));
    page.rebuildHierarchy();
    // Protect a picture even when a broad text region encloses it.
    page.regions.first().bounds = QRect(35, 30, 520, 65);
    page.imageRegions.append({"photo", photograph, {}, true});
    yagf::core::OcrRegion tableRegion;
    tableRegion.id = "table"; tableRegion.type = "table"; tableRegion.bounds = table;
    page.regions.append(tableRegion);
    book.replacePage(page);
    const auto snapshot = book.toJson();
    const QString destination = directory.filePath("formatted.pdf");
    std::atomic_bool cancel{false};
    assert(PdfExporter::write(book, destination, false, cancel, {}, PdfExporter::Mode::FormattedText).status ==
           PdfExportResult::Status::Success);
    assert(book.toJson() == snapshot); // Export estimates never change the project.
    QProcess command;
    command.start("pdftotext", {"-bbox", destination, "-"});
    assert(command.waitForFinished(10000) && command.exitCode() == 0);
    const auto xml = command.readAllStandardOutput();
    assert(!xml.contains("OriginalHeading") && !xml.contains("LeftOriginal"));
    QXmlStreamReader boxes(xml);
    QSet<QString> found;
    while (!boxes.atEnd()) {
        boxes.readNext();
        if (!boxes.isStartElement() || boxes.name() != QLatin1String("word")) continue;
        const auto attributes = boxes.attributes();
        const QRectF box(attributes.value("xMin").toDouble() * 150 / 72,
                         attributes.value("yMin").toDouble() * 150 / 72,
                         (attributes.value("xMax").toDouble() - attributes.value("xMin").toDouble()) * 150 / 72,
                         (attributes.value("yMax").toDouble() - attributes.value("yMin").toDouble()) * 150 / 72);
        const QString text = boxes.readElementText();
        found.insert(text);
        const QRectF expected = text.startsWith("Left") ? page.words[1].bounds : text.startsWith("Right") ?
            page.words[2].bounds : text == "CorrectedHeading" ? page.words[0].bounds : page.words[3].bounds;
        assert(expected.adjusted(-2, -8, 2, 8).contains(box));
    }
    assert(!boxes.hasError());
    assert(found.contains("CorrectedHeading") && found.contains("LeftAlpha") && found.contains("LeftBeta") &&
           found.contains("LeftGamma") && found.contains("RightAlpha") && found.contains("RightBeta") && found.contains("InferredText"));
    if (!QStandardPaths::findExecutable("pdffonts").isEmpty() && QFontDatabase::families().contains("Liberation Serif")) {
        command.start("pdffonts", {destination});
        assert(command.waitForFinished(10000) && command.exitCode() == 0);
        const auto fonts = command.readAllStandardOutput();
        assert(fonts.contains("LiberationSerif-Bold") && fonts.contains("LiberationMono-BoldItalic"));
    }
    if (!QStandardPaths::findExecutable("pdftoppm").isEmpty()) {
        const QString prefix = directory.filePath("formatted-render");
        command.start("pdftoppm", {"-singlefile", "-r", "150", "-png", destination, prefix});
        assert(command.waitForFinished(10000) && command.exitCode() == 0);
        const QImage rendered(prefix + ".png");
        assert(!rendered.isNull());
        assert(rendered.pixelColor(500, 60).blue() > 240 && rendered.pixelColor(500, 60).red() < 15);
        int blackPixels = 0, greenPixels = 0;
        for (int y = 0; y < 500; ++y) for (int x = 0; x < 600; ++x) {
            const QColor pixel = rendered.pixelColor(x, y);
            assert(!(pixel.red() > 100 && pixel.red() > pixel.green() * 2 && pixel.red() > pixel.blue() * 2));
            if (pixel.red() < 70 && pixel.green() < 70 && pixel.blue() < 70) {
                ++blackPixels;
                bool inText = false;
                for (const auto &word : page.words) inText |= word.bounds.adjusted(-1, -1, 1, 1).contains(x, y);
                assert(inText);
            }
            greenPixels += table.contains(x, y) && pixel.green() > pixel.red() + 20 && pixel.green() > pixel.blue() + 20;
        }
        assert(blackPixels > 100 && greenPixels > 100);
    }
}

static void coarseBlockChecks(const QTemporaryDir &directory)
{
    if (QStandardPaths::findExecutable("tesseract").isEmpty()) return;
    QProcess available;
    available.start("tesseract", {"--list-langs"});
    if (!available.waitForFinished(10000) || !available.readAllStandardOutput().split('\n').contains("eng")) return;
    QImage image(700, 400, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::black);
    QFont font(QStringLiteral("Liberation Serif"));
    font.setPixelSize(32); font.setBold(true); painter.setFont(font);
    painter.drawText(QPoint(90, 70), "Original Printed Heading");
    font.setPixelSize(24); font.setBold(false); painter.setFont(font);
    const QStringList lines{"The original document contains several words", "This paragraph remains at its source position",
                            "Common fonts preserve readable printed text"};
    int baseline = 150;
    for (const auto &line : lines) { painter.drawText(QPoint(35, baseline), line); baseline += 40; }
    painter.end();
    const QString source = directory.filePath("coarse-source.png");
    assert(image.save(source));
    auto document = sampleDocument(image.width(), image.height(), source);
    auto &word = document.page(1)->words[0];
    word.layoutBlock = true; word.bounds = image.rect(); word.sourceBoxes = {word.bounds};
    word.originalText = QStringLiteral("Original Printed Heading\n") + lines.join('\n');
    word.text = word.originalText; word.text.replace("Original", "Corrected");
    const QString path = directory.filePath("coarse-formatted.pdf");
    std::atomic_bool cancel{false};
    assert(PdfExporter::write(document, path, false, cancel, {}, PdfExporter::Mode::FormattedText).status ==
           PdfExportResult::Status::Success);
    QProcess extract;
    extract.start("pdftotext", {"-bbox", path, "-"});
    assert(extract.waitForFinished(10000) && extract.exitCode() == 0);
    QXmlStreamReader xml(extract.readAllStandardOutput());
    bool title = false, body = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("word")) continue;
        const double y = xml.attributes().value("yMin").toDouble() * 150 / 72;
        const double x = xml.attributes().value("xMin").toDouble() * 150 / 72;
        const auto text = xml.readElementText();
        if (text == "Corrected") { title = true; assert(x >= 85 && x < 100 && y > 35 && y < 75); }
        if (text == "The") { body = true; assert(x >= 30 && x < 45 && y > 115 && y < 155); }
    }
    assert(title && body && !xml.hasError());
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir directory;
    assert(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("source.png"));
    QImage image(400, 120, QImage::Format_RGB32);
    image.fill(Qt::white);
    image.setPixelColor(2, 2, Qt::red);
    assert(image.save(imagePath));
    OcrDocument document = sampleDocument(400, 120, imagePath);
    document.page(1)->words[0].text = QStringLiteral("Corrected");
    const QString pdfPath = directory.filePath(QStringLiteral("output.pdf"));
    std::atomic_bool cancelled = false;
    const auto result = PdfExporter::write(document, pdfPath, false, cancelled);
    assert(result.status == PdfExportResult::Status::Success);
    const QString samplesPath = directory.filePath("corrections.jsonl");
    using yagf::processing::VerifiedSampleExporter;
    assert(VerifiedSampleExporter::write(document, samplesPath, cancelled).status ==
           PdfExportResult::Status::Error);
    assert(!QFileInfo::exists(samplesPath));
    document.page(1)->words[0].reviewed = true;
    assert(VerifiedSampleExporter::write(document, samplesPath, cancelled).status ==
           PdfExportResult::Status::Success);
    QFile samples(samplesPath);
    assert(samples.open(QIODevice::ReadOnly));
    const QByteArray saved = samples.readAll();
    samples.close();
    const auto record = QJsonDocument::fromJson(saved).object();
    assert(record["original_text"] == "Mistake" && record["text"] == "Corrected");
    assert(record["page"].toInt() == 1 && record["box"].toArray() == QJsonArray({2, 2, 110, 30}));
    const QImage crop = QImage::fromData(QByteArray::fromBase64(record["png_base64"].toString().toLatin1()));
    assert(crop.size() == QSize(110, 30) && crop.pixelColor(0, 0) == Qt::red);
    std::atomic_bool stop = false;
    const auto partial = VerifiedSampleExporter::write(document, samplesPath, stop,
        [&stop](int, int) { stop = true; });
    assert(partial.status == PdfExportResult::Status::Cancelled);
    assert(samples.open(QIODevice::ReadOnly) && samples.readAll() == saved);
    samples.close();
    QProcess extract;
    extract.start(QStringLiteral("pdftotext"), {pdfPath, QStringLiteral("-")});
    assert(extract.waitForFinished(10'000));
    assert(extract.exitCode() == 0);
    const QByteArray text = extract.readAllStandardOutput();
    assert(text.contains("Corrected"));
    assert(!text.contains("Mistake"));
    // Real vision OCR returns whole multiline blocks. Their PDF text must not
    // lose spaces or letters when paragraphs wrap inside one source rectangle.
    OcrDocument passage = sampleDocument(400, 120, imagePath);
    auto &block = passage.page(1)->words[0];
    block.bounds = QRect(0, 0, 400, 120);
    block.sourceBoxes = {block.bounds};
    block.layoutBlock = true;
    block.text = "Executive Mansion\nWashington, August 23, 1864.\n\n";
    for (int i = 0; i < 40; ++i)
        block.text += QStringLiteral("election%1 Administration Union first fire flight flow office affinity ").arg(i);
    const QString passagePdf = directory.filePath("passage.pdf");
    assert(PdfExporter::write(passage, passagePdf, false, cancelled).status == PdfExportResult::Status::Success);
    extract.start("pdftotext", {passagePdf, "-"});
    assert(extract.waitForFinished(10000) && extract.exitCode() == 0);
    const auto passageText = extract.readAllStandardOutput();
    assert(passageText.contains("Executive Mansion") && passageText.contains("Washington"));
    for (int i = 0; i < 40; ++i) assert(passageText.contains("election" + QByteArray::number(i)));
    assert(passageText.count("Administration") == 40 && passageText.count("Union") == 40);
    for (const auto word : {"first", "fire", "flight", "flow", "office", "affinity"})
        assert(passageText.count(word) == 40);
    assert(QString::fromUtf8(passageText).simplified() == block.text.simplified());
    const QString formattedPassage = directory.filePath("formatted-passage.pdf");
    assert(PdfExporter::write(passage, formattedPassage, false, cancelled, {}, PdfExporter::Mode::FormattedText).status ==
           PdfExportResult::Status::Success);
    extract.start("pdftotext", {formattedPassage, "-"});
    assert(extract.waitForFinished(10000) && extract.exitCode() == 0);
    assert(QString::fromUtf8(extract.readAllStandardOutput()).simplified() == block.text.simplified());
    formattedPdfChecks(directory);
    coarseBlockChecks(directory);
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
    // Raster export uses the prepared page, including EXIF orientation and saved rotation.
    oriented.page(1)->preprocessing.quarterTurns = 1;
    oriented.page(1)->width = 40;
    oriented.page(1)->height = 20;
    const QString rotatedPng = directory.filePath("rotated.png");
    assert(yagf::processing::writeScanPage(oriented, 1, rotatedPng, "PNG", cancelled).status ==
           PdfExportResult::Status::Success);
    assert(QImage(rotatedPng).size() == QSize(40, 20));
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
        pages.page(2)->words[0].reviewed = true;
        assert(VerifiedSampleExporter::write(pages, samplesPath, cancelled).status ==
               PdfExportResult::Status::Success);
        assert(samples.open(QIODevice::ReadOnly));
        const auto secondRecord = QJsonDocument::fromJson(samples.readAll()).object();
        assert(secondRecord["page"].toInt() == 2 && secondRecord["text"] == "SecondFix");
        samples.close();
        const QString multiPdf = directory.filePath(QStringLiteral("multi.pdf"));
        assert(PdfExporter::write(pages, multiPdf, false, cancelled).status == PdfExportResult::Status::Success);
        extract.start(QStringLiteral("pdftotext"), {multiPdf, QStringLiteral("-")});
        assert(extract.waitForFinished(10'000));
        assert(extract.exitCode() == 0);
        const QByteArray multiText = extract.readAllStandardOutput();
        assert(multiText.contains("FirstFix"));
        assert(multiText.contains("SecondFix"));
        const QString scanPdf = directory.filePath("scan-multi.pdf");
        assert(PdfExporter::write(pages, scanPdf, false, cancelled, {}, PdfExporter::Mode::ImageOnly).status ==
               PdfExportResult::Status::Success);
        extract.start("pdftotext", {scanPdf, "-"});
        assert(extract.waitForFinished(10000) && extract.exitCode() == 0);
        assert(extract.readAllStandardOutput() == QByteArray("\f\f"));
        const QString formattedMulti = directory.filePath("formatted-multi.pdf");
        assert(PdfExporter::write(pages, formattedMulti, false, cancelled, {}, PdfExporter::Mode::FormattedText).status ==
               PdfExportResult::Status::Success);
        extract.start("pdftotext", {formattedMulti, "-"});
        assert(extract.waitForFinished(10000) && extract.exitCode() == 0);
        const auto formattedText = extract.readAllStandardOutput();
        assert(formattedText.contains("FirstFix") && formattedText.contains("SecondFix"));
        std::atomic_bool stopAfterFirst = false;
        const QString cancelledPath = directory.filePath(QStringLiteral("cancelled.pdf"));
        const auto interrupted = PdfExporter::write(pages, cancelledPath, false, stopAfterFirst,
            [&stopAfterFirst](int completed, int) { if (completed == 1) stopAfterFirst = true; });
        assert(interrupted.status == PdfExportResult::Status::Cancelled);
        assert(!QFileInfo::exists(cancelledPath));
        QFile previous(cancelledPath);
        assert(previous.open(QIODevice::WriteOnly));
        previous.write("Existing destination"); previous.close();
        stopAfterFirst = false;
        assert(PdfExporter::write(pages, cancelledPath, false, stopAfterFirst,
            [&stopAfterFirst](int completed, int) { if (completed == 1) stopAfterFirst = true; },
            PdfExporter::Mode::FormattedText).status == PdfExportResult::Status::Cancelled);
        assert(previous.open(QIODevice::ReadOnly) && previous.readAll() == "Existing destination");
    }
    cancelled = true;
    assert(PdfExporter::write(document, pdfPath, false, cancelled).status == PdfExportResult::Status::Cancelled);
}
