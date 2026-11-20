#include "smartblocks.h"
#include "compactassignment.h"
#include "mathocrengine.h"
#include "tesseractengine.h"

#include <QPainter>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace yagf::processing {
namespace {
using core::OcrWord;

struct Row {
    QRect box;
    QString content;
    QVector<OcrWord> words;
    bool recovered = false;
};

int median(QVector<int> values)
{
    auto middle = values.begin() + values.size()/2;
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

void restoreRecoveredText(core::OcrPage &page, const QVector<Row> &rows)
{
    int nextLine = 0;
    for (const auto &word : page.words) nextLine = std::max(nextLine, word.line);
    for (const auto &row : rows) {
        if (!row.recovered || row.words.isEmpty()) continue;
        struct Line {
            int first, end;
            QRect box;
            QVector<int> centers, heights;
        };
        QVector<Line> lines;
        for (int i = 0; i < page.words.size(); ++i) {
            const auto &word = page.words[i];
            if (i == 0 || word.block != page.words[i-1].block ||
                word.paragraph != page.words[i-1].paragraph || word.line != page.words[i-1].line)
                lines.append({i, i, {}, {}, {}});
            auto &line = lines.last();
            line.end = i+1;
            line.box |= word.bounds;
            line.centers.append(word.bounds.center().y());
            line.heights.append(word.bounds.height());
        }
        const Line *nearest = nullptr;
        qint64 best = std::numeric_limits<qint64>::max();
        for (const auto &line : lines) {
            const qint64 dx = std::max({0, line.box.left()-row.box.right(), row.box.left()-line.box.right()});
            const qint64 dy = median(line.centers)-row.box.center().y();
            const qint64 distance = dx*dx + 4*dy*dy;
            if (distance < best) { best = distance; nearest = &line; }
        }
        int insertion = page.words.size(), block = 1, paragraph = 1, lineNumber = ++nextLine;
        if (nearest) {
            const auto &anchor = page.words[nearest->first];
            block = anchor.block;
            paragraph = anchor.paragraph;
            const int center = median(nearest->centers);
            if (std::abs(center-row.box.center().y()) <= std::max(2, median(nearest->heights)/2)) {
                lineNumber = anchor.line;
                insertion = nearest->first;
                while (insertion < nearest->end && page.words[insertion].bounds.center().x() < row.box.center().x())
                    ++insertion;
            } else insertion = row.box.center().y() < center ? nearest->first : nearest->end;
        }
        for (auto word : row.words) {
            if (!word.layoutBlock || word.id.isEmpty()) {
                word.id = page.id + "/recovered-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
                word.sourceWordIds.clear();
            }
            word.block = block; word.paragraph = paragraph; word.line = lineNumber;
            page.words.insert(insertion++, std::move(word));
        }
    }
}

QVector<core::ImageRegion> textBlocksFromReading(const core::OcrPage &page,
    const QVector<core::ImageRegion> &explicitRegions, const QVector<core::ImageRegion> &detected)
{
    QVector<core::ImageRegion> result = explicitRegions;
    QVector<QRect> paragraphs;
    QHash<QString, bool> formulas;
    for (const auto &word : page.words) formulas.insert(word.id, word.layoutBlock);
    for (const auto &region : page.regions) for (const auto &paragraph : region.paragraphs) {
        if (std::ranges::any_of(explicitRegions, [&](const auto &r) { return r.bounds.intersects(paragraph.bounds); }))
            continue;
        paragraphs.append(paragraph.bounds);
        bool math = false, text = false;
        for (const auto &line : paragraph.lines) for (const auto &id : line.wordIds) {
            if (formulas.value(id)) math = true;
            else text = true;
        }
        result.append({QUuid::createUuid().toString(QUuid::WithoutBraces), paragraph.bounds, {}, false,
            math ? (text ? "mixed" : "math") : "text", true});
    }
    // Keep proposals beside manually drawn blocks where a raw paragraph crosses
    // an explicit boundary. Other prose outlines follow the raw paragraphs.
    for (int i = explicitRegions.size(); i < detected.size(); ++i) {
        if (!std::ranges::any_of(paragraphs, [&](const auto &box) { return box.intersects(detected[i].bounds); }))
            result.append(detected[i]);
    }
    return result;
}

// Bound the actual ink, including accents and scripts outside a Tesseract word box.
QRect inkBox(const QImage &gray, QRect area)
{
    int left = area.right() + 1, top = area.bottom() + 1, right = -1, bottom = -1;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        const auto *line = gray.constScanLine(y);
        for (int x = area.left(); x <= area.right(); ++x) if (line[x] < 175) {
            left = std::min(x, left); right = std::max(x, right);
            top = std::min(y, top); bottom = y;
        }
    }
    return right < left ? QRect{} : QRect(QPoint(left, top), QPoint(right, bottom));
}

bool hasMathOperator(const QString &text)
{
    // Index page ranges and footnote markers are ordinary text. In particular,
    // a dash between two page numbers alone is not evidence of an equation.
    static const QRegularExpression operators(QStringLiteral(
        "[=+*/^<>×÷±∓∑∏∫√≤≥≠≈≡∞∂∇→²³₀-₉−]|_(?=[\\p{L}\\d{])"));
    return text.contains(operators);
}

bool referenceText(const QString &text)
{
    static const QRegularExpression reference(QStringLiteral(
        "^\\s*(?:\\d+[.,;]?\\s*(?:[-–—]\\s*\\d+[.,;]?\\s*)?)+[*)]?\\s*$|"
        "^\\s*\\(?\\d+\\s*[*†‡]+\\s*\\)?[.,;:]?\\s*$|^\\s*[*†‡]+\\s*$"));
    return reference.match(text).hasMatch();
}

bool proseWord(const QString &text)
{
    if (compactAssignment(text)) return false;
    static const QRegularExpression letters(QStringLiteral("\\p{L}{2,}"));
    static const QRegularExpression cyrillicWord(QStringLiteral("[А-Яа-яЁё]{2,}"));
    static const QRegularExpression functions(QStringLiteral("^(sin|cos|tan|log|ln|lim|exp|max|min)$"));
    // OCR often joins a Russian word to a footnote star or an inline operator.
    // That punctuation must not turn the whole word into a formula candidate.
    if (text.contains(cyrillicWord)) return true;
    if (hasMathOperator(text)) return false;
    static const QStringList shortWords{"и", "в", "с", "к", "у", "а", "о", "я"};
    return (text.contains(letters) && !functions.match(text).hasMatch()) || shortWords.contains(text.toLower());
}

bool fractionBar(const QImage &gray, const QRect &box, int left, int right, int y)
{
    const auto hasInk = [&](int row) {
        const auto *pixels = gray.constScanLine(row);
        return std::any_of(pixels + left, pixels + right, [](uchar p) { return p < 175; });
    };
    if (hasInk(y - 1)) return false;
    int end = y + 1;
    while (end < box.bottom() && hasInk(end) && end - y <= (right - left) / 3) ++end;
    if (hasInk(end)) return false;
    bool above = false, below = false;
    for (int row = y - 2; !above && row >= box.top(); --row) above = hasInk(row);
    for (int row = end + 1; !below && row <= box.bottom(); ++row) below = hasInk(row);
    return above && below;
}

bool mathInk(const QImage &gray, const QRect &box)
{
    // A fraction bar has ink above and below, with white space between
    // the bar and the numerator and denominator.
    const int length = std::max(6, box.height() / 5);
    for (int y = box.top() + 1; y < box.bottom(); ++y) {
        const auto *line = gray.constScanLine(y);
        int x = box.left();
        while (x <= box.right()) {
            while (x <= box.right() && line[x] >= 175) ++x;
            const int start = x;
            while (x <= box.right() && line[x] < 175) ++x;
            if (x - start >= length && fractionBar(gray, box, start, x, y)) return true;
        }
    }
    return false;
}

bool ruledDiagram(const QImage &gray, const QRect &box)
{
    // Axis fragments can look like equations to the model. A rule spanning
    // almost the whole crop needs a numerator and denominator to be math.
    const int length = std::max(box.height()*6, box.width()*4/5);
    for (int y = box.top()+1; y < box.bottom(); ++y) {
        const auto *pixels = gray.constScanLine(y);
        int x = box.left();
        while (x <= box.right()) {
            while (x <= box.right() && pixels[x] >= 175) ++x;
            const int start = x;
            while (x <= box.right() && pixels[x] < 175) ++x;
            if (x-start >= length && !fractionBar(gray, box, start, x, y)) return true;
        }
    }
    return false;
}

QRect formulaFromHint(const QImage &gray, const QRect &hint, const QRect &area,
                      const QVector<OcrWord> &words, int glyphHeight)
{
    const QRect bandArea = QRect(area.left(), hint.top()-2, area.width(), hint.height()+4).intersected(area);
    QImage band = gray.copy(bandArea);
    using LineKey = std::tuple<int, int, int>;
    std::set<LineKey> proseLines;
    for (const auto &word : words)
        if (proseWord(word.text)) proseLines.insert({word.block, word.paragraph, word.line});
    {
        QPainter mask(&band);
        for (const auto &word : words)
            if (proseLines.contains({word.block, word.paragraph, word.line}))
                mask.fillRect(word.bounds.translated(-bandArea.topLeft()), Qt::white);
    }
    const auto columnHasInk = [&](int x) {
        for (int y = 0; y < band.height(); ++y) if (band.constScanLine(y)[x] < 175) return true;
        return false;
    };
    int left = hint.left()-bandArea.left(), right = hint.right()-bandArea.left();
    // Recover adjacent symbols such as "w =" that lie outside the image hint,
    // stopping at the gutter before an equation number or another column.
    const int gap = std::max(32, glyphHeight*3);
    for (int x = left-1; x >= 0 && left-x <= gap; --x)
        if (columnHasInk(x)) left = x;
    for (int x = right+1; x < band.width() && x-right <= gap; ++x)
        if (columnHasInk(x)) right = x;
    const QRect box = inkBox(band, {left, 0, right-left+1, band.height()});
    return box.isEmpty() ? QRect{} : box.translated(bandArea.topLeft());
}

void classify(Row &row, const QImage &gray, const QString &mode)
{
    if (mode == "text") { row.content = mode; return; }
    QString nativeText;
    for (const auto &word : row.words) nativeText += word.text + ' ';
    if (mode != "math" && ((!row.words.isEmpty() && referenceText(nativeText)) || ruledDiagram(gray, row.box))) {
        row.content = "text";
        return;
    }
    const auto makeFormula = [](QRect box) {
        OcrWord word;
        word.layoutBlock = true;
        word.bounds = box;
        return word;
    };
    const auto assignments = compactAssignmentWords(row.words);
    const auto prose = [&](int i) { return !assignments.contains(i) && proseWord(row.words[i].text); };
    bool hasProse = false;
    for (int i = 0; i < row.words.size(); ++i) hasProse |= prose(i);
    if (mode == "math" || (!hasProse && (mathInk(gray, row.box) ||
        std::ranges::any_of(row.words, [&](const auto &word) { return hasMathOperator(word.text); })))) {
        row.content = "math";
        row.words = {makeFormula(row.box)};
        return;
    }
    row.content = "text";
    if (!hasProse) return;
    QVector<OcrWord> result;
    int left = row.box.left(), first = 0;
    for (int i = 0; i <= row.words.size(); ++i) {
        if (i < row.words.size() && !prose(i)) continue;
        const int right = i == row.words.size() ? row.box.right() : row.words[i].bounds.left() - 1;
        const QRect gap = inkBox(gray, QRect(QPoint(left, row.box.top()), QPoint(right, row.box.bottom())));
        QString tokens;
        for (int j = first; j < i; ++j) tokens += row.words[j].text;
        if (!gap.isEmpty() && !referenceText(tokens) && (hasMathOperator(tokens) ||
            (gap.width() >= gap.height() && mathInk(gray, gap) && (tokens.isEmpty() || i-first > 1)))) {
            result.append(makeFormula(gap));
            row.content = "mixed";
        } else {
            for (int j = first; j < i; ++j) result.append(row.words[j]);
        }
        if (i < row.words.size()) {
            result.append(row.words[i]);
            left = row.words[i].bounds.right() + 1;
            first = i + 1;
        }
    }
    row.words = std::move(result);
}

QVector<Row> rowsIn(const QImage &gray, QRect area, const QVector<OcrWord> &words,
                    const QString &mode, int glyphHeight, std::stop_token stop, const QVector<core::ImageRegion> &excluded)
{
    QVector<Row> rows;
    // Partition on white gutters in both axes. A short vertical gap can be
    // internal to a fraction or a superscript and belongs to the same row.
    const int maxGap = std::max(3, glyphHeight * 3 / 5);
    // Closely spaced prose lines may have a smaller gap than a fraction bar.
    // Preserve native boundaries between prose and equations as well. Otherwise
    // a descender or scan speck can join a display equation to the next paragraph.
    using LineKey = std::tuple<int, int, int>;
    struct TextLine {
        QRect box;
        QVector<int> centers;
        QVector<int> heights;
        QVector<int> proseCenters;
        QVector<int> proseHeights;
    };
    std::map<LineKey, TextLine> textLines;
    for (const auto &word : words) {
        if (area.contains(word.bounds.center())) {
            auto &line = textLines[{word.block, word.paragraph, word.line}];
            line.box |= word.bounds;
            line.centers.append(word.bounds.center().y());
            line.heights.append(word.bounds.height());
            if (proseWord(word.text)) {
                line.proseCenters.append(word.bounds.center().y());
                line.proseHeights.append(word.bounds.height());
            }
        }
    }
    QSet<int> breaks;
    QRect previous;
    int previousCenter = 0, previousHeight = 0;
    bool previousProse = false;
    for (const auto &[key, line] : textLines) {
        const bool prose = !line.proseCenters.isEmpty();
        const int center = median(prose ? line.proseCenters : line.centers);
        const int height = median(prose ? line.proseHeights : line.heights);
        // Tall OCR boxes and scan speckles can bridge adjacent prose lines.
        // Their typical word centers still identify separate baselines.
        const int distance = center - previousCenter;
        const int typicalHeight = std::max(previousHeight, height);
        if (!previous.isEmpty() && (prose || previousProse) && distance > typicalHeight/2 && distance <= 2*typicalHeight &&
            previous.left() <= line.box.right() && line.box.left() <= previous.right())
            breaks.insert((previousCenter + center + 1) / 2);
        previous = line.box;
        previousCenter = center;
        previousHeight = height;
        previousProse = prose;
    }
    int top = -1, bottom = -1;
    auto finish = [&] {
        QVector<bool> columns(area.width());
        for (int y = top; y <= bottom; ++y) {
            const auto *line = gray.constScanLine(y) + area.left();
            for (int x = 0; x < columns.size(); ++x) if (line[x] < 175) columns[x] = true;
        }
        QSet<int> edges;
        for (const auto &region : excluded) {
            if (region.bounds.bottom() < top || region.bounds.top() > bottom) continue;
            edges.insert(region.bounds.left()-area.left());
            edges.insert(region.bounds.right()+1-area.left());
        }
        int start = -1, last = -1;
        for (int x = 0; x <= columns.size(); ++x) {
            if (start >= 0 && (x == columns.size() || edges.contains(x) || x-last > std::max(32, glyphHeight*3))) {
                auto box = inkBox(gray, QRect(area.left()+start, top, last-start+1, bottom-top+1));
                if (box.width() >= 2 && box.height() >= 2) {
                    Row row{box, {}, {}};
                    for (const auto &word : words) if (box.contains(word.bounds.center())) row.words.append(word);
                    std::ranges::stable_sort(row.words, {}, [](const auto &word) { return word.bounds.left(); });
                    if (!row.words.isEmpty() || (box.width() >= std::max(4, glyphHeight/3) &&
                        box.height() >= std::max(3, glyphHeight/4))) {
                        classify(row, gray, mode);
                        rows.append(std::move(row));
                    }
                }
                start = -1;
            }
            if (x < columns.size() && columns[x]) {
                if (start < 0) start = x;
                last = x;
            }
        }
    };
    for (int y = area.top(); y <= area.bottom() && !stop.stop_requested(); ++y) {
        if (top >= 0 && breaks.contains(y)) { finish(); top = -1; }
        const auto *line = gray.constScanLine(y);
        const bool ink = std::any_of(line + area.left(), line + area.right() + 1,
                                     [](uchar pixel) { return pixel < 175; });
        if (ink) {
            if (top < 0) top = y;
            bottom = y;
        } else if (top >= 0 && y-bottom > maxGap) {
            finish(); top = -1;
        }
    }
    if (top >= 0 && !stop.stop_requested()) finish();
    return rows;
}
}

std::expected<core::OcrPage, OcrError> recognizeSmartBlocks(const QImage &image, const OcrOptions &options,
    const QVector<core::ImageRegion> &regions, bool splitOnly, std::stop_token stop, bool includeUnmarked,
    const core::OcrPage *initialReading, QStringList *warnings, IOcrEngine *textProvider)
{
    QImage gray(image.size(), QImage::Format_Grayscale8);
    gray.fill(Qt::white);
    {
        QPainter painter(&gray);
        painter.drawImage(0, 0, image);
    }
    QImage uncovered = gray;
    if (includeUnmarked && !regions.isEmpty()) {
        QPainter mask(&uncovered);
        for (const auto &region : regions) mask.fillRect(region.bounds, Qt::white);
    }
    core::OcrPage page;
    page.number = options.pageNumber;
    page.id = QStringLiteral("page-%1").arg(page.number);
    page.width = page.sourceWidth = gray.width();
    page.height = page.sourceHeight = gray.height();
    page.engineId = QStringLiteral("local-mixed");
    page.language = TesseractEngine::modelLanguage(options.language);
    page.imageRegions = regions;
    QVector<Row> rows;
    std::map<QString, core::OcrPage> baselines;
    if (initialReading) baselines.emplace(page.language, *initialReading);
    TesseractEngine tesseract;
    IOcrEngine &prose = textProvider ? *textProvider : tesseract;
    const auto analyze = [&](const QImage &pixels, QRect area, const QString &mode, QString language,
                             const QVector<core::ImageRegion> &excluded)
            -> std::expected<QVector<Row>, OcrError> {
        if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
        if (area.isEmpty()) return QVector<Row>{};
        QVector<OcrWord> local;
        QVector<int> heights;
        if (mode != "math") {
            language = TesseractEngine::modelLanguage(language.isEmpty() ? page.language : language);
            if (!baselines.contains(language)) {
                if (initialReading && inkBox(pixels, area).isEmpty()) return QVector<Row>{};
                auto settings = options;
                settings.language = language;
                auto result = prose.recognize(image, settings, stop);
                if (!result) return std::unexpected(result.error());
                baselines.emplace(language, std::move(*result));
            }
            for (const auto &word : baselines.at(language).words) {
                if (!area.contains(word.bounds.center())) continue;
                if (std::ranges::any_of(excluded, [&](const auto &r) { return r.bounds.contains(word.bounds.center()); }))
                    continue;
                local.append(word);
                heights.append(word.bounds.height());
            }
        }
        int height = std::clamp(qRound(image.dotsPerMeterY() * 0.0254 * 0.12), 12, 128);
        if (!heights.isEmpty()) {
            auto median = heights.begin() + heights.size()/2;
            std::nth_element(heights.begin(), median, heights.end());
            height = *median;
        }
        QImage analysis = pixels;
        QVector<Row> hinted;
        if (mode.isEmpty() && baselines.contains(language)) {
            for (const auto &hint : baselines.at(language).imageRegions) {
                if (!hint.autoDetected || !hint.content.isEmpty() || !area.contains(hint.bounds) ||
                    std::ranges::any_of(excluded, [&](const auto &r) { return r.bounds.intersects(hint.bounds); }) ||
                    !mathInk(pixels, hint.bounds)) continue;
                const QRect box = formulaFromHint(pixels, hint.bounds, area, local, height);
                if (box.isEmpty() || std::ranges::any_of(excluded, [&](const auto &r) { return r.bounds.intersects(box); }))
                    continue;
                Row row{box, {}, {}};
                classify(row, pixels, "math");
                hinted.append(std::move(row));
                QPainter mask(&analysis);
                mask.fillRect(box, Qt::white);
            }
        }
        auto found = rowsIn(analysis, area, local, mode, height, stop, excluded);
        found += hinted;
        std::ranges::stable_sort(found, {}, [](const Row &row) { return std::pair(row.box.top(), row.box.left()); });
        if (!splitOnly) for (auto &row : found) {
            if (!row.words.isEmpty()) continue;
            // Page segmentation can skip a short line altogether. Retry only
            // that text line with an explicit baseline and a white margin.
            const int pad = std::max(8, height/2);
            QImage line(row.box.size() + QSize(2*pad, 2*pad), QImage::Format_Grayscale8);
            line.fill(Qt::white);
            {
                QPainter painter(&line);
                painter.drawImage(QPoint(pad, pad), pixels, row.box);
            }
            auto settings = options;
            settings.language = language;
            settings.singleTextLine = true;
            auto retry = prose.recognize(line, settings, stop);
            if (!retry) return std::unexpected(retry.error());
            for (auto word : retry->words) {
                word.bounds.translate(row.box.topLeft()-QPoint(pad, pad));
                word.bounds = word.bounds.intersected(row.box);
                if (word.bounds.isEmpty()) continue;
                word.sourceBoxes.clear();
                row.words.append(std::move(word));
                row.recovered = true;
            }
            classify(row, pixels, mode);
        }
        return found;
    };
    for (auto &region : page.imageRegions) {
        if (region.notText) continue;
        auto found = analyze(gray, region.bounds.intersected(gray.rect()), region.content, region.language, {});
        if (!found) return std::unexpected(found.error());
        if (region.content.isEmpty() && !found->isEmpty()) {
            region.content = found->first().content;
            for (const auto &row : *found) if (row.content != region.content) region.content = "mixed";
        }
        rows += *found;
    }
    const QRect remaining = includeUnmarked ? inkBox(uncovered, uncovered.rect()) : QRect{};
    auto detected = analyze(uncovered, remaining, {}, page.language, regions);
    if (!detected) return std::unexpected(detected.error());
    int lastText = -1;
    for (const auto &row : *detected) {
        // A rectangular proposal can still span an excluded hole in the mask.
        const auto overlaps = [&](const QRect &box) {
            return std::ranges::any_of(regions, [&](const auto &r) { return box.intersects(r.bounds); });
        };
        if (overlaps(row.box)) { lastText = -1; continue; }
        if (lastText >= 0 && row.content == "text") {
            auto &bounds = page.imageRegions[lastText].bounds;
            const int gap = row.box.top() - bounds.bottom();
            if (gap > 0 && gap <= row.box.height() && std::abs(bounds.left()-row.box.left()) <= row.box.height()
                && !overlaps(bounds.united(row.box))) {
                bounds |= row.box;
                continue;
            }
        }
        lastText = row.content == "text" ? page.imageRegions.size() : -1;
        page.imageRegions.append({QUuid::createUuid().toString(QUuid::WithoutBraces), row.box, {}, false, row.content, true});
    }
    const auto limitBlocks = [&](QVector<core::ImageRegion> &blocks) {
        if (blocks.size() <= 1000) return true;
        if (!initialReading) return false;
        blocks.resize(1000);
        if (warnings) warnings->append(QStringLiteral("Only the first 1000 block outlines are shown."));
        return true;
    };
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    rows += *detected;
    // A normal prose page keeps Tesseract's paragraphs, columns, IDs and reading order.
    const auto withDetectedBlocks = [&] {
        auto original = *initialReading;
        original.imageRegions = page.imageRegions;
        original.words.removeIf([&](const auto &word) {
            return std::ranges::any_of(regions, [&](const auto &r) {
                return r.notText && r.bounds.contains(word.bounds.center());
            });
        });
        original.rebuildHierarchy();
        return original;
    };
    if (baselines.size() == 1 && std::ranges::all_of(rows, [](const auto &row) { return row.content == "text"; })) {
        auto original = baselines.begin()->second;
        original.words.removeIf([&](const auto &word) {
            const auto contains = [&](const auto &r) { return r.bounds.contains(word.bounds.center()); };
            return (!includeUnmarked && !std::ranges::any_of(regions, contains)) ||
                std::ranges::any_of(regions, [&](const auto &r) { return r.notText && contains(r); });
        });
        if (!splitOnly) restoreRecoveredText(original, rows);
        original.rebuildHierarchy();
        original.imageRegions = textBlocksFromReading(original, regions, page.imageRegions);
        if (!limitBlocks(original.imageRegions))
            return std::unexpected(OcrError{QStringLiteral("More than 1000 blocks detected. Select a smaller area.")});
        if (splitOnly) {
            page.imageRegions = original.imageRegions;
            return page;
        }
        return original;
    }
    if (!limitBlocks(page.imageRegions))
        return std::unexpected(OcrError{QStringLiteral("More than 1000 blocks detected. Select a smaller area.")});
    if (splitOnly) return page;
    if (baselines.size() == 1) {
        // Detection may combine several baselines or interleave columns. It can
        // propose replacements, but cannot reorder the native reading.
        auto original = baselines.begin()->second;
        original.words.removeIf([&](const auto &word) {
            const auto contains = [&](const auto &r) { return r.bounds.contains(word.bounds.center()); };
            return (!includeUnmarked && !std::ranges::any_of(regions, contains)) ||
                std::ranges::any_of(regions, [&](const auto &r) { return r.notText && contains(r); });
        });
        QVector<Row> recovered;
        for (auto row : rows) if (row.recovered) {
            row.words.removeIf([](const auto &word) { return word.layoutBlock; });
            recovered.append(std::move(row));
        }
        restoreRecoveredText(original, recovered);
        QList<QRect> boxes;
        QVector<bool> inlineMath;
        QVector<QVector<int>> covered;
        QSet<int> claimed;
        const auto assignments = compactAssignmentWords(original.words);
        for (const auto &row : rows) for (const auto &proposal : row.words) {
            if (!proposal.layoutBlock || boxes.contains(proposal.bounds)) continue;
            const bool forced = std::ranges::any_of(regions, [&](const auto &r) {
                return !r.autoDetected && r.content == "math" && r.bounds.contains(proposal.bounds.center());
            });
            if (!forced && ruledDiagram(gray, proposal.bounds)) continue;
            QVector<int> span;
            QString tokens;
            bool unsafe = false;
            for (int i = 0; i < original.words.size(); ++i) {
                const auto &word = original.words[i];
                if (!proposal.bounds.contains(word.bounds.center())) continue;
                span.append(i);
                tokens += word.text;
                if (claimed.contains(i) || (!forced && !assignments.contains(i) && proseWord(word.text))) unsafe = true;
            }
            if (!forced) {
                static const QRegularExpression operand(QStringLiteral("[\\p{L}\\p{N}]"));
                const bool fraction = mathInk(gray, proposal.bounds);
                if (span.isEmpty()) {
                    // A few strokes cut from a skipped glyph are not a formula.
                    // Unread display fractions still have a complete, wide crop.
                    if (!fraction || proposal.bounds.width()*5 < proposal.bounds.height()*3) unsafe = true;
                } else if (!tokens.contains(operand) && !fraction) unsafe = true;
            }
            // A crop crossing unrelated words or columns cannot replace a
            // contiguous source span safely. Keep that native reading intact.
            if (!span.isEmpty() && span.last()-span.first()+1 != span.size()) unsafe = true;
            if (unsafe) continue;
            boxes.append(proposal.bounds);
            inlineMath.append(row.content == "mixed");
            covered.append(span);
            for (int i : span) claimed.insert(i);
        }
        QStringList failures;
        core::OcrPage formulas;
        if (!boxes.isEmpty()) {
            MathOcrEngine math;
            const bool forced=std::ranges::any_of(regions,[](const auto &r){return !r.autoDetected && r.content=="math";});
            auto result = math.recognizeBoxes(image, options, stop, boxes, &failures,forced ? 300000 : 30000);
            if (!result) {
                if (result.error().cancelled || stop.stop_requested()) return std::unexpected(result.error());
                failures.append(result.error().message);
            } else formulas = std::move(*result);
        }
        QHash<int, OcrWord> replacements;
        QSet<int> removed;
        QVector<Row> insertions;
        for (auto formula : formulas.words) {
            const int crop = boxes.indexOf(formula.bounds);
            if (crop < 0) continue;
            if (inlineMath[crop]) formula.text = "\\(" + formula.text + "\\)";
            formula.originalText = formula.text;
            formula.id = original.id + "/math-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
            formula.sourceWordIds.clear();
            const auto &span = covered[crop];
            if (span.isEmpty()) {
                insertions.append({formula.bounds, "math", {formula}, true});
                continue;
            }
            const auto &anchor = original.words[span.first()];
            formula.block = anchor.block;
            formula.paragraph = anchor.paragraph;
            formula.line = anchor.line;
            for (int i : span) {
                formula.sourceWordIds += original.words[i].sourceWordIds;
                removed.insert(i);
            }
            replacements.insert(span.first(), std::move(formula));
        }
        QVector<OcrWord> words;
        for (int i = 0; i < original.words.size(); ++i) {
            if (replacements.contains(i)) words.append(replacements.value(i));
            else if (!removed.contains(i)) words.append(original.words[i]);
        }
        original.words = std::move(words);
        restoreRecoveredText(original, insertions);
        // Display equations need their own export/editing regions. Split only
        // at those boundaries; retain native paragraphs and lines within prose.
        int nextBlock = 0;
        for (const auto &word : original.words) nextBlock = std::max(nextBlock, word.block);
        int previousSourceBlock = -1, proseBlock = -1;
        QSet<int> usedBlocks;
        bool afterDisplay = false;
        for (auto &word : original.words) {
            const int sourceBlock = word.block;
            if (word.layoutBlock && !word.text.startsWith("\\(")) {
                word.block = ++nextBlock;
                afterDisplay = true;
            } else {
                if (afterDisplay || sourceBlock != previousSourceBlock) {
                    proseBlock = usedBlocks.contains(sourceBlock) ? ++nextBlock : sourceBlock;
                    usedBlocks.insert(sourceBlock);
                }
                word.block = proseBlock;
                afterDisplay = false;
            }
            previousSourceBlock = sourceBlock;
        }
        original.regions.clear();
        original.rebuildHierarchy();
        if (!formulas.words.isEmpty()) original.engineId = page.engineId;
        QSet<QString> formulaIds;
        for (const auto &word : original.words) if (word.layoutBlock) formulaIds.insert(word.id);
        for (auto &region : original.regions) {
            bool allMath = true;
            for (const auto &paragraph : region.paragraphs) for (const auto &line : paragraph.lines)
                for (const auto &id : line.wordIds) if (!formulaIds.contains(id)) allMath = false;
            if (allMath) region.type = "equation";
        }
        original.imageRegions = textBlocksFromReading(original, regions, page.imageRegions);
        if (!limitBlocks(original.imageRegions))
            return std::unexpected(OcrError{QStringLiteral("More than 1000 blocks detected. Select a smaller area.")});
        if (warnings && !failures.isEmpty())
            warnings->append(failures.join('\n') + QStringLiteral(" Kept native OCR for failed equation crops."));
        return original;
    }
    std::ranges::stable_sort(rows, {}, [](const Row &row) { return std::pair(row.box.top(), row.box.left()); });
    QList<QRect> boxes;
    QList<int> indices;
    QStringList types;
    for (const auto &row : rows) {
        if (row.words.isEmpty()) continue;
        types.append(row.content);
        for (auto word : row.words) {
            if (word.layoutBlock) {
                boxes.append(word.bounds);
                indices.append(page.words.size());
            }
            word.id.clear(); word.sourceWordIds.clear();
            word.block = types.size(); word.line = word.paragraph = 1;
            page.words.append(std::move(word));
        }
    }
    if (!boxes.isEmpty()) {
        MathOcrEngine math;
        auto formulas = math.recognizeBoxes(image, options, stop, boxes,nullptr,30000);
        if (formulas && formulas->words.size() != boxes.size())
            formulas = std::unexpected(OcrError{QStringLiteral("Local math OCR returned an incomplete set of crops.")});
        if (!formulas) {
            if (!initialReading || formulas.error().cancelled || stop.stop_requested()) return std::unexpected(formulas.error());
            if (warnings) warnings->append(formulas.error().message + QStringLiteral(" Kept the Tesseract reading and block labels."));
            return withDetectedBlocks();
        }
        for (int i = 0; i < indices.size(); ++i) {
            auto &word = page.words[indices[i]];
            word.text = formulas->words[i].text;
            if (types[word.block-1] == "mixed") word.text = "\\(" + word.text + "\\)";
            word.originalText = word.text;
            word.readingEngine = formulas->words[i].readingEngine;
        }
    }
    page.rebuildHierarchy();
    for (int i = 0; i < types.size(); ++i)
        page.regions[i].type = types[i] == "math" ? QStringLiteral("equation") : QStringLiteral("text");
    return page;
}
}
