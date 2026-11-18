#include "pdfexporter.h"
#include "exportpage.h"
#include "pagesource.h"

#include "imageprocessor.h"
#include "correctionmemory.h"
#include "fontestimator.h"
#include "tesseractengine.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <expected>
#include <optional>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QImageReader>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QRegion>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextLayout>
#include <QTextOption>
#include <QTextBoundaryFinder>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <unistd.h>

namespace yagf::processing {
namespace {

PdfExportResult failure(const QString &error)
{
    return {.status = PdfExportResult::Status::Error, .error = error};
}

PdfExportResult runProcess(const QString &program, const QStringList &arguments,
                           const std::atomic_bool &cancelled)
{
    if (QStandardPaths::findExecutable(program).isEmpty())
        return failure(QStringLiteral("%1 is not installed or is not on PATH.").arg(program));
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(10'000))
        return failure(QStringLiteral("Could not start %1: %2").arg(program, process.errorString()));
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning) {
        if (cancelled.load()) {
            process.kill();
            process.waitForFinished(2000);
            return {.status = PdfExportResult::Status::Cancelled};
        }
        if (timer.elapsed() > 300'000) {
            process.kill();
            process.waitForFinished(2000);
            return failure(QStringLiteral("%1 timed out.").arg(program));
        }
        process.waitForFinished(100);
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return failure(QStringLiteral("%1 failed: %2").arg(program,
            QString::fromUtf8(process.readAllStandardError()).trimmed()));
    return {.status = PdfExportResult::Status::Success};
}

void drawWord(QPainter &painter, const core::OcrWord &word)
{
    if (word.text.isEmpty() || word.bounds.isEmpty()) return;
    QFont font;
    if (!word.fontFamily.isEmpty()) font.setFamily(word.fontFamily);
    // Optional ligature glyphs can lose their Unicode mapping in the PDF text layer.
    font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::PreferNoShaping));
    const int fontPixels = word.sourceFontSizePx > 0
        ? qRound(double(word.sourceFontSizePx) * std::max(1, word.fontSizePt) / std::max(1, word.sourceFontSizePt)) : 0;
    font.setPixelSize(std::clamp(fontPixels > 0 ? fontPixels : word.bounds.height(), 6, 512));
    font.setBold(word.bold);
    font.setItalic(word.italic);
    if (word.layoutBlock || word.text.contains('\n') || word.text.contains('\t')) {
        // Reflow passages with normal character spacing inside their source block.
        font.setPixelSize(fontPixels > 0 ? std::clamp(fontPixels, 8, 512) :
            std::clamp(word.fontSizePt > 0 ? qRound(word.fontSizePt * 150.0 / 72.0) : 18, 8, 48));
        QString text = word.text;
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace('\r', '\n');
        text.replace('\n', QChar(0x2028));
        QTextLayout layout(text, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        qreal height = 0;
        layout.beginLayout();
        while (true) {
            auto line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(word.bounds.width());
            line.setPosition(QPointF(0, height));
            height += line.height();
        }
        layout.endLayout();
        const qreal scale = std::min<qreal>(1, word.bounds.height() / std::max<qreal>(1, height));
        painter.save();
        painter.translate(word.bounds.topLeft());
        painter.scale(scale, scale);
        layout.draw(&painter, QPointF(0, 0));
        painter.restore();
        return;
    }
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    const qreal naturalWidth = std::max<qreal>(1.0, metrics.horizontalAdvance(word.text));
    const qreal scaleX = static_cast<qreal>(word.bounds.width()) / naturalWidth;
    painter.save();
    painter.translate(word.bounds.x(), word.bounds.y());
    painter.scale(scaleX, 1.0);
    painter.drawText(QPointF(0, metrics.ascent()), word.text);
    painter.restore();
}

QPageSize pageSizeFor(const QImage &image)
{
    constexpr double pointsPerPixel = 72.0 / 150.0;
    return QPageSize(QSizeF(image.width() * pointsPerPixel,
                            image.height() * pointsPerPixel), QPageSize::Point);
}

QFont formattedFont(const core::OcrWord &word)
{
    QFont font;
    if (!word.fontFamily.isEmpty()) font.setFamily(word.fontFamily);
    font.setBold(word.bold);
    font.setItalic(word.italic);
    font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::PreferNoShaping));
    const int pixels = word.sourceFontSizePx > 0
        ? qRound(double(word.sourceFontSizePx) * std::max(1, word.fontSizePt) / std::max(1, word.sourceFontSizePt))
        : word.layoutBlock ? qRound(std::max(8, word.fontSizePt) * 150.0 / 72.0) : qRound(word.bounds.height() * 1.3);
    font.setPixelSize(std::clamp(pixels, 6, 512));
    return font;
}

// Fit by changing font size and wrapping at the block width. Scaling a completed
// paragraph also shrinks its width and moves its lines away from the source layout.
void drawFormattedWord(QPainter &painter, const core::OcrWord &word)
{
    if (word.text.isEmpty() || word.bounds.isEmpty()) return;
    QFont font = formattedFont(word);
    painter.save();
    painter.setClipRect(word.bounds, Qt::IntersectClip);
    const bool acrossLines = std::ranges::any_of(word.sourceBoxes, [&](const QRect &box) {
        return !word.sourceBoxes.isEmpty() && std::abs(box.center().y() - word.sourceBoxes.first().center().y()) >
            std::min(box.height(), word.sourceBoxes.first().height()) / 2;
    });
    if (!word.layoutBlock && !word.text.contains('\n') && acrossLines) {
        // A correction joined across lines still belongs to its separate image fragments.
        QVector<int> boundaries{0};
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, word.text);
        for (int end = finder.toNextBoundary(); end >= 0; end = finder.toNextBoundary()) boundaries.append(end);
        qint64 totalWidth = 0, usedWidth = 0;
        for (const auto &box : word.sourceBoxes) totalWidth += box.width();
        int start = 0;
        for (int i = 0; i < word.sourceBoxes.size(); ++i) {
            usedWidth += word.sourceBoxes[i].width();
            const int count = boundaries.size() - 1;
            const int end = i + 1 == word.sourceBoxes.size() ? word.text.size() :
                boundaries[std::clamp(int(count * usedWidth / std::max<qint64>(1, totalWidth)), 0, count)];
            auto fragment = word;
            fragment.bounds = word.sourceBoxes[i];
            fragment.sourceBoxes.clear();
            fragment.text = word.text.mid(start, end - start);
            drawFormattedWord(painter, fragment);
            start = end;
        }
    } else if (word.layoutBlock || word.text.contains('\n') || word.text.contains('\t')) {
        QString text = word.text;
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace('\r', '\n');
        text.replace('\n', QChar(0x2028));
        QRectF inkBounds;
        const auto makeLayout = [&](const QFont &candidate) {
            auto layout = std::make_unique<QTextLayout>(text, candidate);
            QTextOption option;
            option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            layout->setTextOption(option);
            const QFontMetricsF metrics(candidate);
            inkBounds = {};
            qreal y = 0;
            layout->beginLayout();
            while (true) {
                auto line = layout->createLine();
                if (!line.isValid()) break;
                line.setLineWidth(word.bounds.width());
                line.setPosition(QPointF(0, y));
                const QString content = text.mid(line.textStart(), line.textLength()).trimmed();
                if (!content.isEmpty()) inkBounds = inkBounds.united(metrics.tightBoundingRect(content)
                    .translated(0, y + line.ascent()));
                y += line.height();
            }
            layout->endLayout();
            return layout;
        };
        auto layout = makeLayout(font);
        const auto fits = [&] { return inkBounds.height() <= word.bounds.height() && inkBounds.width() <= word.bounds.width(); };
        if (!fits()) {
            int low = 6, high = font.pixelSize(), best = 6;
            while (low <= high) {
                const int middle = (low + high) / 2;
                font.setPixelSize(middle);
                layout = makeLayout(font);
                if (fits()) { best = middle; low = middle + 1; }
                else high = middle - 1;
            }
            font.setPixelSize(best);
            layout = makeLayout(font);
        }
        const qreal scale = std::min({1.0, word.bounds.width() / std::max(1.0, inkBounds.width()),
                                     word.bounds.height() / std::max(1.0, inkBounds.height())});
        painter.translate(word.bounds.topLeft());
        painter.scale(scale, scale);
        layout->draw(&painter, -inkBounds.topLeft());
    } else {
        const QFontMetricsF metrics(font);
        const QRectF ink = metrics.tightBoundingRect(word.text);
        const qreal scale = std::min({1.0, word.bounds.width() / std::max(1.0, ink.width()),
                                     word.bounds.height() / std::max(1.0, ink.height())});
        painter.translate(word.bounds.topLeft());
        painter.scale(scale, scale);
        painter.setFont(font);
        painter.drawText(-ink.topLeft(), word.text);
    }
    painter.restore();
}

QString normalizedLine(const QString &text)
{
    QString normalized;
    for (QChar c : text.toCaseFolded()) if (c.isLetterOrNumber()) normalized += c;
    return normalized.left(256);
}

double lineSimilarity(const QString &left, const QString &right)
{
    if (left.isEmpty() || right.isEmpty()) return 0;
    if (left == right) return 1;
    if (std::min(left.size(), right.size()) * 3 < std::max(left.size(), right.size())) return 0;
    QVector<int> previous(right.size() + 1), current(right.size() + 1);
    for (int j = 0; j <= right.size(); ++j) previous[j] = j;
    for (int i = 1; i <= left.size(); ++i) {
        current[0] = i;
        for (int j = 1; j <= right.size(); ++j)
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
                                  previous[j - 1] + (left[i - 1] != right[j - 1])});
        previous.swap(current);
    }
    return 1.0 - double(previous.last()) / std::max(left.size(), right.size());
}

QColor paperColor(const QImage &image, const QRect &box);

QRect nearbyInkBounds(const QImage &image, const QRect &box)
{
    const int margin = std::clamp(box.width() / 4, 8, 48);
    const QRect search = box.adjusted(-margin, -1, margin, 1).intersected(image.rect());
    const int threshold = qGray(paperColor(image, search).rgb()) - 25;
    QRect ink;
    for (int y = search.top(); y <= search.bottom(); ++y) for (int x = search.left(); x <= search.right(); ++x) {
        const QColor pixel = image.pixelColor(x, y);
        if (qGray(pixel.rgb()) >= threshold) continue;
        const QRect point(x, y, 1, 1);
        ink = ink.isEmpty() ? point : ink.united(point);
    }
    return ink.isEmpty() ? box : ink;
}

void recoverBlockLayout(core::OcrPage &page, const QImage &image, const std::atomic_bool &cancelled,
                         QSet<QString> &retainedText)
{
    if (QStandardPaths::findExecutable(QStringLiteral("tesseract")).isEmpty()) return;
    QSet<QString> graphicsWords;
    QRegion graphics;
    for (const auto &region : page.regions) {
        if (!region.isFigure() && region.type != "table" && region.type != "formula" && region.type != "equation") continue;
        graphics += region.bounds;
        for (const auto &paragraph : region.paragraphs) for (const auto &line : paragraph.lines)
            for (const auto &id : line.wordIds) graphicsWords.insert(id);
    }
    for (const auto &region : page.imageRegions) if (region.notText) graphics += region.bounds;
    QVector<core::OcrWord> words;
    QVector<QRect> erasedLines;
    int nextBlock = 1;
    for (const auto &word : page.words) nextBlock = std::max(nextBlock, word.block + 1);
    bool changed = false;
    for (const auto &block : page.words) {
        if (cancelled.load()) return;
        const auto tokens = block.text.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
        const auto lines = block.text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
        if (!block.layoutBlock || tokens.size() < 12 || lines.size() < 3 || lines.size() > 256 ||
            graphicsWords.contains(block.id) || !image.rect().contains(block.bounds)) {
            words.append(block); continue;
        }
        const QImage crop = image.copy(block.bounds);
        const int scale = crop.width() < 1200 && crop.height() < 1600 ? 2 : 1;
        QImage input = scale == 1 ? crop : crop.scaled(crop.size() * scale, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        input.setDotsPerMeterX(crop.dotsPerMeterX() * scale);
        input.setDotsPerMeterY(crop.dotsPerMeterY() * scale);
        QString language = page.language;
        if (language.isEmpty() || language == "auto") language = QStringLiteral("eng");
        TesseractEngine layoutEngine;
        // Only the geometry is used; the saved OCR/corrected lines remain verbatim.
        const auto layout = layoutEngine.recognize(input, {.language = language, .pageNumber = page.number}, {}, &cancelled);
        if (cancelled.load()) return;
        if (!layout || layout->words.isEmpty() || layout->words.size() > 2000) { words.append(block); continue; }
        QVector<QVector<core::OcrWord>> sourceLines;
        QString previousKey;
        for (auto word : layout->words) {
            word.bounds = QRectF(double(word.bounds.x()) / scale, double(word.bounds.y()) / scale,
                                 double(word.bounds.width()) / scale, double(word.bounds.height()) / scale)
                              .toAlignedRect().intersected(crop.rect());
            if (word.bounds.isEmpty() || graphics.intersects(word.bounds.translated(block.bounds.topLeft()))) continue;
            const QString key = QStringLiteral("%1/%2/%3").arg(word.block).arg(word.paragraph).arg(word.line);
            if (key != previousKey) { sourceLines.emplaceBack(); previousKey = key; }
            sourceLines.last().append(word);
        }
        core::OcrPage positions;
        positions.width = positions.sourceWidth = crop.width();
        positions.height = positions.sourceHeight = crop.height();
        for (auto &line : sourceLines) {
            QVector<int> heights;
            for (const auto &word : line) heights.append(word.bounds.height());
            std::ranges::sort(heights);
            const int median = heights[heights.size() / 2];
            std::ranges::sort(line, {}, [](const auto &word) { return word.bounds.x(); });
            core::OcrWord item;
            QVector<int> bottoms, sizes;
            const auto append = [&] {
                if (normalizedLine(item.originalText).size() >= 3 && item.bounds.height() >= 4) {
                    std::ranges::sort(bottoms); std::ranges::sort(sizes);
                    const int height = sizes[sizes.size() / 2];
                    item.bounds.setTop(bottoms[bottoms.size() / 2] - height + 1);
                    item.bounds.setHeight(height);
                    item.text = item.originalText;
                    item.block = positions.words.size() + 1; item.paragraph = item.line = 1;
                    positions.words.append(item);
                }
                item = {};
                bottoms.clear(); sizes.clear();
            };
            for (const auto &word : line) {
                if (word.bounds.height() > median * 3 && word.confidence < 50) continue;
                if (!item.bounds.isEmpty() && word.bounds.left() - item.bounds.right() > median * 4) append();
                if (!item.originalText.isEmpty()) item.originalText += ' ';
                item.originalText += word.text;
                item.bounds = item.bounds.united(word.bounds);
                bottoms.append(word.bounds.bottom()); sizes.append(word.bounds.height());
            }
            append();
        }
        if (positions.words.size() < lines.size() || positions.words.size() > 400) { words.append(block); continue; }
        struct Match { int target; int source; double score; };
        QVector<QString> normalizedSources;
        for (const auto &line : positions.words) normalizedSources.append(normalizedLine(line.text));
        QVector<Match> matches;
        for (int i = 0; i < lines.size(); ++i) {
            if (cancelled.load()) return;
            const QString normalized = normalizedLine(lines[i]);
            for (int j = 0; j < positions.words.size(); ++j)
                matches.append({i, j, lineSimilarity(normalized, normalizedSources[j])});
        }
        std::ranges::sort(matches, [](const Match &a, const Match &b) { return a.score > b.score; });
        QVector<int> assignment(lines.size(), -1);
        QVector<double> scores(lines.size(), 0);
        QSet<int> used;
        int strongMatches = 0;
        for (const auto &match : matches) {
            if (assignment[match.target] >= 0 || used.contains(match.source)) continue;
            assignment[match.target] = match.source;
            scores[match.target] = match.score;
            used.insert(match.source);
            strongMatches += match.score >= 0.55;
        }
        // Detailed recovery must be well supported; otherwise fit the original block.
        if (strongMatches * 5 < lines.size() * 4 || assignment.contains(-1)) { words.append(block); continue; }
        // OCR can merge neighbouring printed lines. Bound each recovered line by
        // the next one in its column before choosing its font size.
        for (auto &line : positions.words) {
            line.bounds = nearbyInkBounds(crop, line.bounds);
            int bottom = line.bounds.bottom();
            for (const auto &other : positions.words) {
                if (other.bounds.top() <= line.bounds.top()) continue;
                const int overlap = std::min(line.bounds.right(), other.bounds.right()) -
                                    std::max(line.bounds.left(), other.bounds.left());
                if (overlap * 2 < std::min(line.bounds.width(), other.bounds.width())) continue;
                bottom = std::min(bottom, other.bounds.top() - 1);
            }
            line.bounds.setBottom(std::max(line.bounds.top() + 2, bottom));
        }
        estimateTextFormatting(positions, crop, {}, &cancelled);
        for (int i = 0; i < lines.size(); ++i) {
            auto word = positions.words[assignment[i]];
            const bool preserveHeading = scores[i] < 0.55 && i < 3 && block.text == block.originalText &&
                                         block.fontFamily.isEmpty() && !block.bold && !block.italic;
            if (scores[i] < 0.55) {
                int previous = i - 1, next = i + 1;
                while (previous >= 0 && scores[previous] < 0.55) --previous;
                while (next < lines.size() && scores[next] < 0.55) ++next;
                if (previous >= 0 || next < lines.size()) {
                    const bool usePrevious = previous >= 0 && (next == lines.size() || i - previous <= next - i);
                    const auto &nearby = positions.words[assignment[usePrevious ? previous : next]];
                    word = nearby;
                    int step = std::max(4, nearby.bounds.height() + 2);
                    int y = nearby.bounds.y() + (i - (usePrevious ? previous : next)) * step;
                    if (previous >= 0 && next < lines.size()) {
                        const auto &before = positions.words[assignment[previous]].bounds;
                        const auto &after = positions.words[assignment[next]].bounds;
                        if (std::abs(before.x() - after.x()) < std::max(before.width(), after.width()) / 2 && after.y() > before.y()) {
                            step = std::max(3, (after.y() - before.y()) / (next - previous));
                            y = before.y() + (i - previous) * step;
                        }
                    }
                    word.bounds.moveTop(std::clamp(y, 0, std::max(0, crop.height() - word.bounds.height())));
                    word.bounds.setHeight(std::min(word.bounds.height(), step));
                    word.bounds = nearbyInkBounds(crop, word.bounds);
                }
            }
            if (!preserveHeading)
                erasedLines.append(word.bounds.adjusted(-2, -1, 2, 1).intersected(crop.rect()).translated(block.bounds.topLeft()));
            word.text = lines[i].trimmed();
            word.block = nextBlock++;
            word.id = QStringLiteral("pdf-layout-%1-%2").arg(block.id).arg(i);
            if (preserveHeading) retainedText.insert(word.id);
            word.bounds.translate(block.bounds.topLeft());
            word.sourceBoxes = {word.bounds};
            if (word.fontFamily.isEmpty()) word.fontFamily = QStringLiteral("serif");
            if (word.sourceFontSizePx <= 0) {
                word.sourceFontSizePx = std::max(6, qRound(word.bounds.height() * 1.3));
                word.fontSizePt = word.sourceFontSizePt = std::clamp(qRound(word.sourceFontSizePx * 72.0 / 150), 6, 72);
            }
            if (block.reviewed && !block.fontFamily.isEmpty()) word.fontFamily = block.fontFamily;
            word.bold = word.bold || block.bold;
            word.italic = word.italic || block.italic;
            if (block.sourceFontSizePt > 0 && block.fontSizePt != block.sourceFontSizePt)
                word.fontSizePt = std::clamp(qRound(double(word.fontSizePt) * block.fontSizePt / block.sourceFontSizePt), 6, 72);
            words.append(word);
        }
        changed = true;
    }
    if (changed) {
        page.words = std::move(words);
        page.rebuildHierarchy();
        for (const auto &box : erasedLines) {
            core::OcrRegion area;
            area.bounds = box;
            page.regions.append(area);
        }
    }
}

void estimateExportFonts(core::OcrPage &page, const QImage &image, const std::atomic_bool &cancelled)
{
    core::OcrPage estimates = page;
    for (auto &word : estimates.words) word.reviewed = !word.fontFamily.isEmpty();
    estimateTextFormatting(estimates, image, {}, &cancelled);
    for (int i = 0; i < page.words.size(); ++i) {
        auto &word = page.words[i];
        const auto &estimate = estimates.words[i];
        if (!word.fontFamily.isEmpty() || estimate.fontFamily.isEmpty()) continue;
        word.fontFamily = estimate.fontFamily;
        if (word.sourceFontSizePx <= 0) {
            const bool resized = word.fontSizePt != word.sourceFontSizePt;
            word.sourceFontSizePx = estimate.sourceFontSizePx;
            word.sourceFontSizePt = estimate.sourceFontSizePt;
            if (!resized) word.fontSizePt = estimate.fontSizePt;
        }
        if (!word.reviewed) {
            word.bold = word.bold || estimate.bold;
            word.italic = word.italic || estimate.italic;
        }
    }
}

QColor paperColor(const QImage &image, const QRect &box)
{
    // Sample the surrounding paper so replacing text also works on tinted pages.
    const QRect edge = box.adjusted(-2, -2, 2, 2).intersected(image.rect());
    QVector<int> red, green, blue;
    const auto sample = [&](int x, int y) {
        const QColor color = image.pixelColor(x, y);
        red.append(color.red()); green.append(color.green()); blue.append(color.blue());
    };
    for (int x = edge.left(); x <= edge.right(); x += std::max(1, edge.width() / 64)) {
        sample(x, edge.top()); sample(x, edge.bottom());
    }
    for (int y = edge.top(); y <= edge.bottom(); y += std::max(1, edge.height() / 64)) {
        sample(edge.left(), y); sample(edge.right(), y);
    }
    if (red.isEmpty()) return Qt::white;
    std::ranges::sort(red); std::ranges::sort(green); std::ranges::sort(blue);
    const qsizetype index = red.size() * 3 / 4;
    return QColor(red[index], green[index], blue[index]);
}

bool drawFormattedPage(QPainter &painter, core::OcrPage page, const QImage &image,
                       const std::atomic_bool &cancelled)
{
    QSet<QString> retainedText;
    recoverBlockLayout(page, image, cancelled, retainedText);
    estimateExportFonts(page, image, cancelled);
    if (cancelled.load()) return false;
    QRegion graphics;
    QSet<QString> rasterWords = retainedText;
    QVector<QRect> textAreas;
    for (const auto &region : page.regions) {
        bool retained = false;
        for (const auto &paragraph : region.paragraphs) for (const auto &line : paragraph.lines)
            for (const auto &id : line.wordIds) retained |= retainedText.contains(id);
        // Keep table ruling and mathematical/illustration layout from the scan.
        if (retained || region.isFigure() || region.type == "table" || region.type == "formula" || region.type == "equation") {
            graphics += region.bounds;
            for (const auto &paragraph : region.paragraphs) for (const auto &line : paragraph.lines)
                for (const auto &id : line.wordIds) rasterWords.insert(id);
        } else textAreas.append(region.bounds);
    }
    for (const auto &region : page.imageRegions) if (region.notText) graphics += region.bounds;
    painter.save();
    painter.setClipRegion(QRegion(image.rect()).subtracted(graphics), Qt::IntersectClip);
    for (const auto &area : textAreas) painter.fillRect(area, paperColor(image, area));
    for (const auto &word : page.words) {
        if (rasterWords.contains(word.id)) continue;
        if (word.sourceBoxes.isEmpty()) painter.fillRect(word.bounds, paperColor(image, word.bounds));
        else for (const auto &box : word.sourceBoxes) painter.fillRect(box, paperColor(image, box));
    }
    painter.setPen(Qt::black);
    for (const auto &word : page.words) {
        if (cancelled.load()) { painter.restore(); return false; }
        if (rasterWords.contains(word.id)) {
            // Preserve content-stream reading order for retained raster text too.
            painter.save();
            painter.setClipping(false);
            painter.setOpacity(0);
            drawWord(painter, word);
            painter.restore();
        } else drawFormattedWord(painter, word);
    }
    painter.restore();
    return true;
}

} // namespace

std::expected<QImage, ExportResult> readExportPage(const core::OcrDocument &document, int number,
                                                   const QTemporaryDir &temporary,
                                                   const std::atomic_bool &cancelled)
{
    QString imagePath = document.sourcePath;
    if (isPagedSource(document.sourcePath)) {
        const QString prefix = temporary.filePath(QStringLiteral("page"));
        const auto command = pageRenderCommand(document.sourcePath, number, prefix);
        const auto rendered = runProcess(command.program, command.arguments, cancelled);
        if (rendered.status != PdfExportResult::Status::Success) return std::unexpected(rendered);
        imagePath = command.outputPath;
    }
    QImageReader reader(imagePath);
    reader.setAutoTransform(true);
    const QImage source = reader.read();
    if (source.isNull())
        return std::unexpected(failure(QStringLiteral("Could not read page %1: %2").arg(number).arg(reader.errorString())));
    const core::OcrPage *page = document.page(number);
    if (page && (source.width() != page->sourceWidth || source.height() != page->sourceHeight))
        return std::unexpected(failure(QStringLiteral("Source dimensions changed on page %1.").arg(number)));
    auto processed = ImageProcessor::apply(source,
        page ? page->preprocessing : core::PreprocessSettings{});
    if (!processed) return std::unexpected(failure(processed.error()));
    if (page && (processed->width() != page->width || processed->height() != page->height))
        return std::unexpected(failure(QStringLiteral("Processed dimensions changed on page %1.").arg(number)));
    return std::move(*processed);
}

PdfExportResult PdfExporter::write(const core::OcrDocument &document,
                                   const QString &destination, bool archive,
                                   const std::atomic_bool &cancelled, const Progress &progress, Mode mode)
{
    if (document.sourcePath.isEmpty() || document.totalPages < 1 || destination.isEmpty())
        return failure(QStringLiteral("A source document and destination are required."));
    if (archive && QStandardPaths::findExecutable(QStringLiteral("ocrmypdf")).isEmpty())
        return failure(QStringLiteral("OCRmyPDF is required for PDF/A export."));
    QTemporaryDir temporary;
    if (!temporary.isValid()) return failure(QStringLiteral("Could not create temporary export files."));
    const QString rawPdf = temporary.filePath(QStringLiteral("searchable.pdf"));
    const QString archivePdf = temporary.filePath(QStringLiteral("archive.pdf"));
    {
        QPdfWriter writer(rawPdf);
        writer.setResolution(150);
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setTitle(QStringLiteral("YAGF searchable document"));
        writer.setCreator(QStringLiteral("YAGF 2"));
        QPainter painter;
        for (int number = 1; number <= document.totalPages; ++number) {
            if (cancelled.load()) return {.status = PdfExportResult::Status::Cancelled};
            auto processed = readExportPage(document, number, temporary, cancelled);
            if (!processed) return processed.error();
            if (isPagedSource(document.sourcePath)) {
                processed->setDotsPerMeterX(5906);
                processed->setDotsPerMeterY(5906);
            }
            const auto *page = document.page(number);
            if (number == 1) {
                writer.setPageSize(pageSizeFor(*processed));
                if (!painter.begin(&writer)) return failure(QStringLiteral("Could not start PDF output."));
            } else {
                writer.setPageSize(pageSizeFor(*processed));
                if (!writer.newPage()) return failure(QStringLiteral("Could not add PDF page %1.").arg(number));
            }
            painter.drawImage(QRect(0, 0, processed->width(), processed->height()), *processed);
            if (page && mode == Mode::FormattedText) {
                if (!drawFormattedPage(painter, *page, *processed, cancelled))
                    return {.status = PdfExportResult::Status::Cancelled};
            } else if (page) {
                painter.save();
                painter.setPen(Qt::black);
                painter.setOpacity(0.0);
                for (const core::OcrWord &word : page->words) drawWord(painter, word);
                painter.restore();
            }
            if (progress) progress(number, document.totalPages);
        }
        painter.end();
    }
    if (cancelled.load()) return {.status = PdfExportResult::Status::Cancelled};
    if (!QFileInfo::exists(rawPdf) || QFileInfo(rawPdf).size() == 0)
        return failure(QStringLiteral("PDF output is empty."));
    QString completedPath = rawPdf;
    if (archive) {
        const auto converted = runProcess(QStringLiteral("ocrmypdf"),
            {QStringLiteral("--skip-text"), QStringLiteral("--output-type"), QStringLiteral("pdfa"),
             QStringLiteral("--optimize"), QStringLiteral("0"), rawPdf, archivePdf}, cancelled);
        if (converted.status != PdfExportResult::Status::Success) return converted;
        completedPath = archivePdf;
    }
    if (cancelled.load()) return {.status = PdfExportResult::Status::Cancelled};
    const QString staged = destination + QLatin1Char('.') +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".tmp");
    if (!QFile::copy(completedPath, staged)) {
        QFile::remove(staged);
        return failure(QStringLiteral("Could not write PDF to %1.").arg(destination));
    }
    if (cancelled.load()) {
        QFile::remove(staged);
        return {.status = PdfExportResult::Status::Cancelled};
    }
    const QByteArray stagedName = QFile::encodeName(staged);
    const QByteArray destinationName = QFile::encodeName(destination);
    if (::rename(stagedName.constData(), destinationName.constData()) != 0) {
        const QString error = QString::fromLocal8Bit(std::strerror(errno));
        QFile::remove(staged);
        return failure(QStringLiteral("Could not complete PDF export: %1").arg(error));
    }
    return {.status = PdfExportResult::Status::Success};
}

ExportResult VerifiedSampleExporter::write(const core::OcrDocument &document, const QString &destination,
                                       const std::atomic_bool &cancelled, const PdfExporter::Progress &progress)
{
    QTemporaryDir temporary;
    if (!temporary.isValid()) return failure(QStringLiteral("Could not create temporary export files."));
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) return failure(output.errorString());
    int count = 0;
    int completed = 0;
    for (const auto &page : document.pages) {
        std::optional<QImage> image;
        for (const auto &word : page.words) {
            if (cancelled.load()) return {.status = ExportResult::Status::Cancelled};
            if (!core::isVerifiedCorrection(word)) continue;
            if (!image) {
                auto loaded = readExportPage(document, page.number, temporary, cancelled);
                if (!loaded) return loaded.error();
                image = std::move(*loaded);
            }
            if (!image->rect().contains(word.bounds))
                return failure(QStringLiteral("Word outside the image on page %1.").arg(page.number));
            QByteArray png;
            QBuffer buffer(&png);
            if (!buffer.open(QIODevice::WriteOnly) || !image->copy(word.bounds).save(&buffer, "PNG"))
                return failure(QStringLiteral("Could not encode a word on page %1.").arg(page.number));
            const QJsonObject sample{
                {"page", page.number}, {"word_id", word.id}, {"source_word_id", word.sourceWordIds.first()},
                {"engine", page.engineId}, {"language", page.language}, {"confidence", word.confidence},
                {"box", QJsonArray{word.bounds.x(), word.bounds.y(), word.bounds.width(), word.bounds.height()}},
                {"original_text", word.originalText}, {"text", word.text},
                {"png_base64", QString::fromLatin1(png.toBase64())}
            };
            const QByteArray row = QJsonDocument(sample).toJson(QJsonDocument::Compact) + '\n';
            if (output.write(row) != row.size()) return failure(output.errorString());
            ++count;
        }
        ++completed;
        if (progress) progress(completed, document.pages.size());
    }
    if (cancelled.load()) return {.status = ExportResult::Status::Cancelled};
    if (!count) return failure(QStringLiteral("No verified single-word corrections to export."));
    if (!output.commit()) return failure(output.errorString());
    return {.status = ExportResult::Status::Success};
}

} // namespace yagf::processing
