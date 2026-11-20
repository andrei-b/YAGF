#include "pdfpagelayout.h"
#include "pagebackground.h"
#include "fontestimator.h"

#include <QRegularExpression>
#include <algorithm>

namespace yagf::processing::detail {
namespace {

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
                         QSet<QString> &retainedText, QVector<QRect> &erasedLines, const LayoutRecognizer &recognize)
{
    if (!recognize) return;
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
    int nextBlock = 1;
    for (const auto &word : page.words) nextBlock = std::max(nextBlock, word.block + 1);
    bool changed = false;
    for (const auto &block : page.words) {
        if (cancelled.load()) return;
        const auto tokens = block.text.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
        const auto lines = block.text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
        if (!block.layoutBlock || tokens.size() < 12 || lines.size() < 3 || lines.size() > 256 ||
            block.text.contains(QStringLiteral("\\(")) || block.text.contains(QStringLiteral("\\[")) ||
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
        // Only the geometry is used; the saved OCR/corrected lines remain verbatim.
        const auto layout = recognize(input, language, page.number);
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
                word.fontSizePt = word.sourceFontSizePt = std::clamp(qRound(word.sourceFontSizePx * 72.0 / page.renderDpi), 6, 72);
            }
            if (block.reviewed && !block.fontFamily.isEmpty()) word.fontFamily = block.fontFamily;
            word.bold = word.bold || block.bold;
            word.italic = word.italic || block.italic;
            if (block.sourceFontSizePx > 0) {
                // Recovering line positions must not reintroduce per-line size
                // estimates into a passage with a consistent recognized font.
                word.sourceFontSizePx = block.sourceFontSizePx;
                word.sourceFontSizePt = block.sourceFontSizePt;
                word.fontSizePt = block.fontSizePt;
            } else if (block.sourceFontSizePt > 0 && block.fontSizePt != block.sourceFontSizePt)
                word.fontSizePt = std::clamp(qRound(double(word.fontSizePt) * block.fontSizePt / block.sourceFontSizePt), 6, 72);
            words.append(word);
        }
        changed = true;
    }
    if (changed) {
        page.words = std::move(words);
        page.rebuildHierarchy();
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

} // namespace

PdfPageLayout preparePdfPage(const core::OcrPage &source, const QImage &image,
                             const std::atomic_bool &cancelled, const LayoutRecognizer &recognize)
{
    PdfPageLayout result;
    result.page = source;
    auto &page = result.page;
    QVector<QRect> erasedLines;
    QSet<QString> retainedText;
    recoverBlockLayout(page, image, cancelled, retainedText, erasedLines, recognize);
    estimateExportFonts(page, image, cancelled);
    if (cancelled.load()) return result;
    auto &graphics = result.graphics;
    auto &rasterWords = result.rasterWords;
    rasterWords = retainedText;
    auto &textAreas = result.textAreas;
    QSet<QString> nonemptyWords;
    for (const auto &word : page.words)
        if (!word.text.trimmed().isEmpty()) nonemptyWords.insert(word.id);
    for (const auto &region : page.regions) {
        bool retained = false;
        bool hasText = false;
        for (const auto &paragraph : region.paragraphs) for (const auto &line : paragraph.lines)
            for (const auto &id : line.wordIds) {
                retained |= retainedText.contains(id);
                hasText |= nonemptyWords.contains(id);
            }
        // Retain the original artwork and table ruling. Formulas are set separately.
        const bool emptyEquation = (region.type == "formula" || region.type == "equation")
            && !hasText;
        if (retained || region.isFigure() || region.type == "table" || emptyEquation) {
            graphics += region.bounds;
            for (const auto &paragraph : region.paragraphs) for (const auto &line : paragraph.lines)
                for (const auto &id : line.wordIds) rasterWords.insert(id);
        } else textAreas.append(region.bounds);
    }
    for (const auto &region : page.imageRegions) if (region.notText) graphics += region.bounds;
    textAreas += erasedLines;
    return result;
}

} // namespace yagf::processing::detail
