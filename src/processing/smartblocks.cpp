#include "smartblocks.h"
#include "mathocrengine.h"
#include "tesseractengine.h"

#include <QPainter>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <map>

namespace yagf::processing {
namespace {
using core::OcrWord;

struct Row {
    QRect box;
    QString content;
    QVector<OcrWord> words;
};

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
    static const QRegularExpression operators(QStringLiteral(
        "[=+*/^<>×÷±∓∑∏∫√≤≥≠≈≡∞∂∇→²³₀-₉−]|(?<=\\d)-(?=\\d)|_(?=[\\p{L}\\d{])"));
    return text.contains(operators);
}

bool proseWord(const QString &text)
{
    static const QRegularExpression letters(QStringLiteral("\\p{L}{2,}"));
    static const QRegularExpression functions(QStringLiteral("^(sin|cos|tan|log|ln|lim|exp|max|min)$"));
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

void classify(Row &row, const QImage &gray, const QString &mode)
{
    if (mode == "text") { row.content = mode; return; }
    const auto makeFormula = [](QRect box) {
        OcrWord word;
        word.layoutBlock = true;
        word.bounds = box;
        return word;
    };
    const bool hasProse = std::ranges::any_of(row.words, [](const auto &word) { return proseWord(word.text); });
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
        if (i < row.words.size() && !proseWord(row.words[i].text)) continue;
        const int right = i == row.words.size() ? row.box.right() : row.words[i].bounds.left() - 1;
        const QRect gap = inkBox(gray, QRect(QPoint(left, row.box.top()), QPoint(right, row.box.bottom())));
        QString tokens;
        for (int j = first; j < i; ++j) tokens += row.words[j].text;
        if (!gap.isEmpty() && (hasMathOperator(tokens) ||
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
    // Preserve baseline OCR line breaks when both lines contain prose.
    using LineKey = std::tuple<int, int, int>;
    std::map<LineKey, QRect> textLines;
    for (const auto &word : words) {
        if (area.contains(word.bounds.center()) && proseWord(word.text))
            textLines[{word.block, word.paragraph, word.line}] |= word.bounds;
    }
    QSet<int> breaks;
    QRect previous;
    for (const auto &[key, line] : textLines) {
        if (!previous.isEmpty() && previous.bottom() < line.top() && line.top()-previous.bottom() <= maxGap+2 &&
            previous.left() <= line.right() && line.left() <= previous.right())
            breaks.insert((previous.bottom() + line.top() + 1) / 2);
        previous = line;
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
    const core::OcrPage *initialReading, QStringList *warnings)
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
    bool recoveredWords = false;
    TesseractEngine tesseract;
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
                auto result = tesseract.recognize(image, settings, stop);
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
        auto found = rowsIn(pixels, area, local, mode, height, stop, excluded);
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
            auto retry = tesseract.recognize(line, settings, stop);
            if (!retry) return std::unexpected(retry.error());
            for (auto word : retry->words) {
                word.bounds.translate(row.box.topLeft()-QPoint(pad, pad));
                word.bounds = word.bounds.intersected(row.box);
                if (word.bounds.isEmpty()) continue;
                word.sourceBoxes.clear();
                row.words.append(std::move(word));
                recoveredWords = true;
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
    if (page.imageRegions.size() > 1000) {
        if (!initialReading)
            return std::unexpected(OcrError{QStringLiteral("More than 1000 blocks detected. Select a smaller area.")});
        page.imageRegions.resize(1000);
        if (warnings) warnings->append(QStringLiteral("Only the first 1000 block outlines are shown."));
    }
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    if (splitOnly) return page;
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
    if (initialReading && baselines.size() == 1 && !recoveredWords &&
        std::ranges::all_of(rows, [](const auto &row) { return row.content == "text"; }))
        return withDetectedBlocks();
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
        auto formulas = math.recognizeBoxes(image, options, stop, boxes);
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
        }
    }
    page.rebuildHierarchy();
    for (int i = 0; i < types.size(); ++i)
        page.regions[i].type = types[i] == "math" ? QStringLiteral("equation") : QStringLiteral("text");
    return page;
}
}
