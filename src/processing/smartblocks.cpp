#include "smartblocks.h"
#include "regiongeometry.h"
#include "mathsymbols.h"
#include "recognitionrecorder.h"
#include "formulagrowth.h"
#include "compactassignment.h"
#include "mathocrengine.h"
#include "tesseractengine.h"

#include <QPainter>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <set>

namespace yagf::processing {
namespace {
using core::OcrWord;

// Own the early formula work until it has been consumed or cancelled. In
// particular, a failed text read must not leave an inference running behind it.
class FormulaPrefetch {
    struct Result {
        std::expected<core::OcrPage, OcrError> reading;
        QStringList warnings;
    };
    std::stop_source stop;
    std::stop_callback<std::function<void()>> parentStop;
    std::future<Result> pending;
    QList<QRect> prefetched;
    std::optional<Result> cached;
    QHash<QRect,QRegion> masks;

    static std::expected<core::OcrPage,OcrError> read(IOcrEngine *provider,const QImage &image,
        const OcrOptions &options,std::stop_token stop,const QList<QRect> &boxes,QStringList *warnings,
        int budget,const QHash<QRect,QRegion> &masks)
    {
        if (std::ranges::none_of(boxes,[&](QRect b){return masks.contains(b);}))
            return recognizeFormulaBoxes(provider,image,options,stop,boxes,warnings,budget);
        QElapsedTimer timer; timer.start();
        core::OcrPage result;
        for (QRect box : boxes) {
            QImage input = image;
            if (masks.contains(box)) {
                input = image.convertToFormat(QImage::Format_RGB32);
                QPainter painter(&input);
                painter.setClipRegion(QRegion(box)-masks.value(box));
                painter.fillRect(box,Qt::white);
            }
            auto one = recognizeFormulaBoxes(provider,input,options,stop,{box},warnings,
                                              std::max(0,budget-int(timer.elapsed())));
            if (!one) return std::unexpected(one.error());
            if (masks.contains(box)) {
                for (auto &word : one->words) {
                    word.sourceBoxes.clear();
                    for (QRect part : masks.value(box).intersected(word.bounds)) word.sourceBoxes.append(part);
                }
            }
            result.words += one->words;
            appendRecognitionEvidence(result,*one);
        }
        return result;
    }
public:
    explicit FormulaPrefetch(std::stop_token parent)
        : parentStop(parent, [this] { stop.request_stop(); }) {}
    ~FormulaPrefetch() {
        stop.request_stop();
        if (pending.valid()) pending.wait();
    }
    void setMask(QRect box,const QRegion &shape) { masks.insert(box,shape); }
    bool owns(QRect box,QPoint point) const { return !masks.contains(box) || masks.value(box).contains(point); }
    void start(IOcrEngine *provider, const QImage &image, const OcrOptions &options,
               const QList<QRect> &boxes, int budget) {
        if (boxes.isEmpty()) return;
        prefetched = boxes;
        pending = std::async(std::launch::async, [=, masks=masks, token=stop.get_token()] {
            Result result;
            result.reading = read(provider, image, options, token, boxes, &result.warnings, budget,masks);
            return result;
        });
    }
    std::expected<core::OcrPage, OcrError> recognize(IOcrEngine *provider, const QImage &image,
        const OcrOptions &options, std::stop_token token, const QList<QRect> &boxes,
        QStringList *warnings, int budget) {
        if (!pending.valid() && !cached)
            return read(provider, image, options, token, boxes, warnings, budget,masks);
        if (pending.valid()) cached = pending.get();
        if (token.stop_requested()) return std::unexpected(OcrError{{}, true});
        if (!cached->reading) return std::unexpected(cached->reading.error());
        if (!cached->warnings.isEmpty()) {
            if (!warnings) return std::unexpected(OcrError{cached->warnings.join('\n')});
            *warnings += cached->warnings;
        }
        auto result = *cached->reading;
        QList<QRect> remaining;
        for (const auto &box : boxes) if (!prefetched.contains(box)) remaining.append(box);
        if (!remaining.isEmpty()) {
            auto later = read(provider, image, options, token, remaining, warnings, budget,masks);
            if (!later) return std::unexpected(later.error());
            result.words += later->words;
            appendRecognitionEvidence(result, *later);
        }
        // Preserve crop order even when early and text-dependent crops interleave.
        const auto words = std::move(result.words);
        result.words.clear();
        for (const auto &box : boxes) for (const auto &word : words)
            if (word.bounds == box) { result.words.append(word); break; }
        result.regions.clear();
        result.rebuildHierarchy();
        return result;
    }
};

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
    const QVector<core::ImageRegion> &explicitRegions, const QVector<core::ImageRegion> &detected, const core::RegionInk &ink)
{
    QVector<core::ImageRegion> result = explicitRegions;
    QVector<QRect> paragraphs;
    QHash<QString, bool> formulas;
    for (const auto &word : page.words) formulas.insert(word.id, word.layoutBlock);
    for (const auto &region : page.regions) for (const auto &paragraph : region.paragraphs) {
        if (std::ranges::any_of(explicitRegions, [&](const auto &r) { return ink.shapeConflicts(r.shape(), paragraph.bounds); }))
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
        if (!std::ranges::any_of(paragraphs, [&](const auto &box) { return ink.shapeConflicts(box, detected[i].shape()); }))
            result.append(detected[i]);
    }
    return result;
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
    static const QRegularExpression initials(QStringLiteral("^(?:[А-Яа-яЁё]\\.){2,}$"));
    if (text.contains(cyrillicWord) || initials.match(text).hasMatch()) return true;
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
            for (QRect part : region.shape().intersected(QRect(area.left(),top,area.width(),bottom-top+1))) {
                edges.insert(part.left()-area.left());
                edges.insert(part.right()+1-area.left());
            }
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

struct SymbolEvidence {
    QSet<QRect> crops;
    QSet<QRect> nativeSymbols;
    QHash<QRect,MathSymbol> isolated;
};

void routeMathSymbols(QVector<Row> &rows, const QImage &gray, QRect area, const QString &mode,
    const QVector<OcrWord> &words, const QVector<core::ImageRegion> &regions, bool fullPage,
    SymbolEvidence &evidence, std::stop_token stop)
{
    const auto symbols = findMathSymbols(gray,area,words,mode == "math",stop);
    if (symbols.isEmpty()) return;
    QSet<QRect> anchors;
    for (const auto &symbol : symbols) anchors.insert(symbol.bounds);
    QVector<QRect> protectedBoxes;
    const auto owner = std::ranges::find_if(regions,[&](const auto &r) {
        return r.bounds.intersected(gray.rect()) == area;
    });
    for (const auto &region : regions) {
        if (owner != regions.end() && &region == &*owner) continue;
        const bool neighboringText = fullPage && mode == "mixed" && owner != regions.end() &&
            !region.notText && region.content == "text" && region.language == owner->language;
        if (!neighboringText) for (QRect part : region.shape()) protectedBoxes.append(part);
    }
    for (const auto &word : words) {
        if (anchors.contains(word.bounds) || (mode == "math" && area.contains(word.bounds))) continue;
        if (word.reviewed || word.geometryInferred || word.layoutBlock || proseWord(word.text) ||
            QStringLiteral(".,;:!?").contains(word.text)) protectedBoxes.append(word.bounds);
    }

    // Existing equation spans and visually anchored crops describe the same
    // ink. Coalesce them before dispatch so a limit or operand is read once.
    QVector<QRect> formulas, display;
    for (const auto &row : rows) for (const auto &word : row.words) {
        if (!word.layoutBlock) continue;
        formulas.append(word.bounds);
        if (row.content == "math") display.append(word.bounds);
    }
    QVector<QRect> proposals;
    for (const auto &symbol : symbols) {
        if (stop.stop_requested()) return;
        auto box = growSymbolCrop(gray,symbol.bounds,fullPage ? gray.rect() : area,
                                  protectedBoxes,symbol.largeOperator,stop);
        bool changed;
        do {
            const QRect before = box;
            for (const auto &other : formulas) if (box.intersects(other)) box |= other;
            for (const auto &other : proposals) if (box.intersects(other)) box |= other;
            changed = box != before;
        } while (changed);
        if (std::ranges::any_of(protectedBoxes,[&](QRect b){return b.intersects(box);})) continue;
        // Partial native words could be swallowed during replacement by center.
        if (std::ranges::any_of(words,[&](const auto &w) {
                return w.bounds.intersects(box) && !box.contains(w.bounds);
            })) continue;
        proposals.removeIf([&](QRect b){return box.contains(b);});
        proposals.append(box);
        evidence.nativeSymbols.insert(symbol.bounds);
        if (box == symbol.bounds && !symbol.latex.isEmpty()) evidence.isolated.insert(box,symbol);
    }
    if (proposals.isEmpty()) return;
    for (auto &row : rows) {
        const bool hadWords = !row.words.isEmpty();
        row.words.removeIf([&](const auto &word) {
            return std::ranges::any_of(proposals,[&](QRect box){return box.contains(word.bounds);});
        });
        if (row.words.isEmpty() && (hadWords || std::ranges::any_of(proposals,
                [&](QRect box){return box.contains(row.box);}))) row.box = {};
    }
    rows.removeIf([](const auto &row){return row.box.isEmpty();});
    for (const auto &box : proposals) {
        OcrWord formula;
        formula.bounds = box;
        formula.layoutBlock = true;
        const bool besideProse = std::ranges::any_of(words,[&](const OcrWord &w) {
            const auto center = w.bounds.center();
            const int gap = std::max(w.bounds.left()-box.right(), box.left()-w.bounds.right());
            return gap >= 0 && gap <= 4*w.bounds.height() && area.contains(center) &&
                center.y() >= box.top() && center.y() <= box.bottom() && proseWord(w.text);
        });
        const bool displayMath = mode == "math" || (!besideProse && std::ranges::any_of(display,
            [&](QRect previous){return box.contains(previous);}));
        const auto line = std::ranges::find_if(rows,[&](const Row &row) {
            return !displayMath && !row.words.isEmpty() && row.box.intersects(box) &&
                std::ranges::any_of(row.words,[](const auto &w){return !w.layoutBlock;});
        });
        if (line == rows.end()) rows.append({box,displayMath ? "math" : "mixed",{formula}});
        else {
            line->content = "mixed";
            line->box |= box;
            line->words.append(formula);
            std::ranges::stable_sort(line->words,{},[](const auto &w){return w.bounds.left();});
        }
        evidence.crops.insert(box);
    }
}
}

static void readIsolatedSymbol(OcrWord &word, const SymbolEvidence &evidence,
    const QImage &image, const OcrOptions &options, bool inlineMath, core::RecognitionHistory &history)
{
    const auto symbol = evidence.isolated.constFind(word.bounds);
    if (symbol == evidence.isolated.cend() || !evidence.crops.contains(word.bounds)) return;
    const QString text = inlineMath ? QStringLiteral("\\(%1\\)").arg(symbol->latex) : symbol->latex;
    if (word.text == text) return;
    const auto previous = word.text;
    const auto provider = word.readingEngine;
    word.originalText = word.text = text;
    QStringList historyIds;
    const auto pageCrop = word.bounds.translated(options.historyOffset);
    for (const auto &run : history.runs) {
        if (run.engine != provider || run.status != "succeeded") continue;
        if (run.processedCrop == pageCrop) historyIds.append(run.id);
        else for (const auto &v : run.output["requested_crops"].toArray()) {
            const auto rect = v.toArray();
            if (rect.size() != 4) continue;
            const QRect crop(rect[0].toInt(),rect[1].toInt(),rect[2].toInt(),rect[3].toInt());
            if (crop.translated(run.processedCrop.topLeft()) == pageCrop) {
                historyIds.append(run.id);
                break;
            }
        }
    }
    word.alternative = {provider,previous,{text},true,historyIds};
    word.readingEngine = QStringLiteral("math-glyph-matcher");
    if (options.history) {
        auto settings = croppedOcrOptions(options,word.bounds);
        settings.historyRole = "formula";
        settings.formula = true;
        auto run = options.history->begin(word.readingEngine,"font-templates",image.copy(word.bounds),settings);
        history.runs.append(options.history->finish(std::move(run),symbol->elapsedMs,"succeeded",{},
            {{"model_version","1"},{"latex",symbol->latex},{"error_fraction",symbol->error},
             {"match_margin",symbol->margin},{"model_sha256",symbol->bankHash},
             {"formula_engine",provider},{"formula_reading",previous}}));
    }
}

static std::expected<core::OcrPage, OcrError> recognizeBlocks(const QImage &image, const OcrOptions &options,
    const QVector<core::ImageRegion> &regions, bool splitOnly, std::stop_token stop, bool includeUnmarked,
    const core::OcrPage *initialReading, QStringList *warnings, IOcrEngine *textProvider, IOcrEngine *formulaProvider,
    bool readBaseline, bool parallelFormulas)
{
    // Existing callbacks need not be thread safe. Deliver progress from the two
    // engines one callback at a time, without sharing mutable OCR results.
    auto settings = options;
    std::mutex progressMutex;
    if (options.progress) settings.progress = [&](const QString &stage, int done, int total) {
        std::lock_guard lock(progressMutex);
        options.progress(stage, done, total);
    };
    FormulaPrefetch formulaReadings(stop);
    QImage gray(image.size(), QImage::Format_Grayscale8);
    gray.fill(Qt::white);
    {
        QPainter painter(&gray);
        painter.drawImage(0, 0, image);
    }
    const core::RegionInk ink(gray);
    QImage uncovered = gray;
    if (includeUnmarked && !regions.isEmpty()) {
        QPainter mask(&uncovered);
        for (const auto &region : regions) for (QRect part : region.shape()) mask.fillRect(part, Qt::white);
    }
    core::OcrPage page;
    page.number = options.pageNumber;
    page.id = QStringLiteral("page-%1").arg(page.number);
    page.width = page.sourceWidth = gray.width();
    page.height = page.sourceHeight = gray.height();
    page.engineId = QStringLiteral("local-mixed");
    const bool localText = !textProvider || textProvider->id() == "tesseract";
    page.language = localText ? TesseractEngine::modelLanguage(options.language) : options.language;
    page.imageRegions = regions;
    const bool explicitMath = std::ranges::any_of(regions,[](const auto &r) {
        return !r.notText && !r.autoDetected && r.content == "math";
    });
    const int formulaBudget = explicitMath ? 300000 : 30000;
    QVector<Row> rows;
    SymbolEvidence symbolEvidence;
    std::map<QString, core::OcrPage> baselines;
    if (initialReading) baselines.emplace(page.language, *initialReading);
    TesseractEngine tesseract;
    IOcrEngine &prose = textProvider ? *textProvider : tesseract;
    QSet<QRect> inlineCrops;
    const auto analyze = [&](const QImage &pixels, QRect area, const QString &mode, QString language,
                             const QVector<core::ImageRegion> &excluded, const QRegion *owner=nullptr)
            -> std::expected<QVector<Row>, OcrError> {
        if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
        if (area.isEmpty()) return QVector<Row>{};
        QVector<OcrWord> local;
        QVector<int> heights;
        if (mode != "math") {
            language = language.isEmpty() ? page.language : language;
            if (localText) language = TesseractEngine::modelLanguage(language);
            if (!baselines.contains(language)) {
                if (initialReading && inkBox(pixels, area).isEmpty()) return QVector<Row>{};
                auto textSettings = settings;
                textSettings.language = language;
                auto result = prose.recognize(image, textSettings, stop);
                if (!result) return std::unexpected(result.error());
                baselines.emplace(language, std::move(*result));
            }
            if (owner && !splitOnly) {
                auto cropOptions = croppedOcrOptions(settings,area);
                cropOptions.language = language;
                auto reading = prose.recognize(pixels.copy(area),cropOptions,stop);
                if (!reading) return std::unexpected(reading.error());
                auto &baseline = baselines.at(language);
                QVector<Row> lines;
                std::optional<std::tuple<int,int,int>> previous;
                for (auto word : reading->words) {
                    word.bounds.translate(area.topLeft());
                    for (auto &fragment : word.sourceBoxes) fragment.translate(area.topLeft());
                    if (!owner->intersects(word.bounds)) continue;
                    word.id.clear();
                    word.sourceWordIds.clear();
                    const auto key = std::tuple(word.block,word.paragraph,word.line);
                    if (key != previous) lines.append(Row{{},"text",{},true});
                    previous = key;
                    lines.last().box |= word.bounds;
                    lines.last().words.append(std::move(word));
                }
                if (!lines.isEmpty()) {
                    baseline.words.removeIf([&](const auto &w){return owner->contains(w.bounds.center());});
                    restoreRecoveredText(baseline,lines);
                }
                appendRecognitionEvidence(baseline,*reading,area.topLeft());
                baseline.rebuildHierarchy();
            }
            for (const auto &word : baselines.at(language).words) {
                if (!area.contains(word.bounds.center()) || (owner && !owner->contains(word.bounds.center()))) continue;
                if (std::ranges::any_of(excluded, [&](const auto &r) { return r.shape().contains(word.bounds.center()); }))
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
                    std::ranges::any_of(excluded, [&](const auto &r) { return r.shape().intersects(hint.bounds); }) ||
                    !mathInk(pixels, hint.bounds)) continue;
                const QRect box = formulaFromHint(pixels, hint.bounds, area, local, height);
                if (box.isEmpty() || std::ranges::any_of(excluded, [&](const auto &r) { return r.shape().intersects(box); }))
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
        if (!splitOnly && mode != "text") {
            // Use the page reading for prose guards just beyond a block's edge.
            // Formula prefetch can start before text OCR has supplied any words.
            const auto native = baselines.find(language.isEmpty() ? page.language : language);
            const auto &context = native == baselines.end() ? local : native->second.words;
            routeMathSymbols(found,pixels,area,mode,context,regions,includeUnmarked && !owner,symbolEvidence,stop);
            // Native OCR can read the integral as a brace and each limit as
            // a separate word. Repair its formula proposal before dispatch.
            for (auto &row : found) for (auto &word : row.words) {
                if (!word.layoutBlock) continue;
                const QRect before = word.bounds;
                for (const QRect &sign : boundedIntegralSigns(pixels, before)) {
                    const QRect grown = word.bounds.united(sign);
                    if (grown == word.bounds || ((!includeUnmarked || owner) && !area.contains(grown))) continue;
                    if (std::ranges::any_of(regions, [&](const auto &other) {
                            return other.bounds != area && (other.notText || other.content == "math") &&
                                   ink.shapeConflicts(grown,other.shape());
                        })) continue;
                    if (std::ranges::any_of(context, [&](const auto &other) {
                            return grown.contains(other.bounds.center()) && !before.contains(other.bounds.center()) &&
                                   (other.reviewed || (!sign.contains(other.bounds) && proseWord(other.text)));
                        })) continue;
                    word.bounds = grown;
                    row.box |= grown;
                    symbolEvidence.crops.insert(grown);
                    if (row.content == "mixed") inlineCrops.insert(grown);
                }
            }
        }
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
            auto textSettings = croppedOcrOptions(settings,row.box,pad);
            textSettings.language = language;
            textSettings.singleTextLine = true;
            auto retry = prose.recognize(line, textSettings, stop);
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
    const auto analyzeRegion = [&](const core::ImageRegion &region) -> std::expected<QVector<Row>, OcrError> {
        const QRect box = region.bounds.intersected(gray.rect());
        const bool inlineFormula = region.detectedLabel == "inline_formula" || region.detectedLabel == "inline_formula_geometry";
        if (!box.isEmpty() && region.content == "math" && (inlineFormula || region.detectedLabel == "display_formula" || !region.parts.isEmpty())) {
            // The accepted crop is one expression, even if native OCR gives
            // the numerator and denominator different lines.
            if (inlineFormula) inlineCrops.insert(box);
            if (!region.parts.isEmpty()) formulaReadings.setMask(box,region.shape());
            OcrWord formula;
            formula.bounds = box;
            formula.layoutBlock = true;
            return QVector<Row>{{box, inlineFormula ? "mixed" : "math", {formula}}};
        }
        if (region.parts.isEmpty()) return analyze(gray,box,region.content,region.language,{});
        const QRegion owner = region.shape();
        QImage pixels = gray;
        {
            QPainter mask(&pixels);
            mask.setClipRegion(QRegion(box)-owner);
            mask.fillRect(box,Qt::white);
        }
        auto rows = analyze(pixels,box,region.content,region.language,{},&owner);
        if (rows) for (const auto &row : *rows) for (const auto &word : row.words)
            if (word.layoutBlock) formulaReadings.setMask(word.bounds,owner);
        return rows;
    };
    std::map<int, QVector<Row>> earlyRows;
    if (!splitOnly && parallelFormulas && formulaProvider != &prose) {
        QList<QRect> boxes;
        for (int i = 0; i < page.imageRegions.size(); ++i) {
            const auto &region = page.imageRegions[i];
            if (region.notText || region.content != "math") continue;
            auto found = analyzeRegion(region);
            if (!found) return std::unexpected(found.error());
            for (const auto &row : *found) for (const auto &word : row.words)
                if (word.layoutBlock && !boxes.contains(word.bounds)) boxes.append(word.bounds);
            earlyRows.emplace(i, std::move(*found));
        }
        formulaReadings.start(formulaProvider, image, settings, boxes, formulaBudget);
    }
    std::optional<core::OcrPage> baseline;
    if (readBaseline && !initialReading) {
        auto reading = prose.recognize(image, settings, stop);
        if (!reading) return std::unexpected(reading.error());
        baseline = std::move(*reading);
        initialReading = &*baseline;
        baselines.emplace(page.language, *baseline);
    }
    for (int i = 0; i < page.imageRegions.size(); ++i) {
        auto &region = page.imageRegions[i];
        if (region.notText) continue;
        std::expected<QVector<Row>, OcrError> found = earlyRows.contains(i) ? std::move(earlyRows.at(i)) :
            analyzeRegion(region);
        if (!found) return std::unexpected(found.error());
        if (region.content.isEmpty() && !found->isEmpty()) {
            region.content = found->first().content;
            for (const auto &row : *found) if (row.content != region.content) region.content = "mixed";
        }
        rows += *found;
    }
    auto occupied = regions;
    if (includeUnmarked && !symbolEvidence.crops.isEmpty()) {
        QPainter mask(&uncovered);
        for (const auto &box : symbolEvidence.crops) {
            mask.fillRect(box,Qt::white);
            occupied.append({{},box});
        }
    }
    const QRect remaining = includeUnmarked ? inkBox(uncovered, uncovered.rect()) : QRect{};
    auto detected = analyze(uncovered, remaining, {}, page.language, occupied);
    if (!detected) return std::unexpected(detected.error());
    int lastText = -1;
    for (const auto &row : *detected) {
        // A rectangular proposal can still span an excluded hole in the mask.
        const auto overlaps = [&](const QRect &box) {
            return std::ranges::any_of(regions, [&](const auto &r) { return ink.shapeConflicts(box,r.shape()); });
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
    // Another block may have produced a native row for a borrowed limit.
    // Keep that baseline available for fallback; emit the ink only once.
    if (baselines.size() != 1 && !symbolEvidence.crops.isEmpty()) {
        for (auto &row : rows) row.words.removeIf([&](const OcrWord &w) {
            return !w.layoutBlock && std::ranges::any_of(symbolEvidence.crops,
                [&](QRect box){return box.contains(w.bounds);});
        });
    }
    // A normal prose page keeps Tesseract's paragraphs, columns, IDs and reading order.
    const auto withDetectedBlocks = [&] {
        auto original = *initialReading;
        original.imageRegions = page.imageRegions;
        original.words.removeIf([&](const auto &word) {
            return std::ranges::any_of(regions, [&](const auto &r) {
                return r.notText && r.shape().contains(word.bounds.center());
            });
        });
        original.rebuildHierarchy();
        return original;
    };
    if (baselines.size() == 1 && std::ranges::all_of(rows, [](const auto &row) { return row.content == "text"; })) {
        auto original = baselines.begin()->second;
        original.words.removeIf([&](const auto &word) {
            const auto contains = [&](const auto &r) { return r.shape().contains(word.bounds.center()); };
            return (!includeUnmarked && !std::ranges::any_of(regions, contains)) ||
                std::ranges::any_of(regions, [&](const auto &r) { return r.notText && contains(r); });
        });
        if (!splitOnly) restoreRecoveredText(original, rows);
        original.rebuildHierarchy();
        original.imageRegions = textBlocksFromReading(original, regions, page.imageRegions, ink);
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
            const auto contains = [&](const auto &r) { return r.shape().contains(word.bounds.center()); };
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
                // Applied model classifications keep autoDetected so layout can
                // be refreshed. Honor their Math type even when text OCR reads
                // formula symbols as Cyrillic prose.
                return (!r.autoDetected || !r.detectedLabel.isEmpty()) && r.content == "math" &&
                    r.shape().contains(proposal.bounds.center());
            });
            if (!forced && ruledDiagram(gray, proposal.bounds)) continue;
            QVector<int> span;
            QString tokens;
            bool unsafe = false;
            for (int i = 0; i < original.words.size(); ++i) {
                const auto &word = original.words[i];
                if (!proposal.bounds.contains(word.bounds.center()) || !formulaReadings.owns(proposal.bounds,word.bounds.center())) continue;
                span.append(i);
                tokens += word.text;
                if (claimed.contains(i) || (!forced && !assignments.contains(i) && !symbolEvidence.nativeSymbols.contains(word.bounds) && proseWord(word.text))) unsafe = true;
            }
            if (!forced) {
                static const QRegularExpression operand(QStringLiteral("[\\p{L}\\p{N}]"));
                const bool fraction = mathInk(gray, proposal.bounds);
                if (span.isEmpty()) {
                    // A few strokes cut from a skipped glyph are not a formula.
                    // Unread display fractions still have a complete, wide crop.
                    if (!symbolEvidence.crops.contains(proposal.bounds) &&
                        (!fraction || proposal.bounds.width()*5 < proposal.bounds.height()*3)) unsafe = true;
                } else if (!tokens.contains(operand) && !fraction && !symbolEvidence.crops.contains(proposal.bounds)) unsafe = true;
            }
            // Heuristic crops cannot cross unrelated native reading spans.
            // An explicit/model Formula block has stronger geometry: replace
            // only its covered words even when Tesseract interleaves another
            // row's tokens. Words outside the accepted block stay in place.
            if (!forced && !symbolEvidence.crops.contains(proposal.bounds) && !span.isEmpty() &&
                span.last()-span.first()+1 != span.size()) unsafe = true;
            if (unsafe) continue;
            boxes.append(proposal.bounds);
            inlineMath.append(row.content == "mixed");
            covered.append(span);
            for (int i : span) claimed.insert(i);
        }
        QStringList failures;
        core::OcrPage formulas;
        if (!boxes.isEmpty()) {
            auto result = formulaReadings.recognize(formulaProvider, image, settings, stop, boxes, &failures,formulaBudget);
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
            readIsolatedSymbol(formula,symbolEvidence,image,settings,inlineMath[crop],formulas.recognitionHistory);
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
            if (inlineCrops.contains(formula.bounds))
                insertions.append({formula.bounds,"mixed",{formula},true});
            else replacements.insert(span.first(), std::move(formula));
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
        original.imageRegions = textBlocksFromReading(original, regions, page.imageRegions, ink);
        if (!limitBlocks(original.imageRegions))
            return std::unexpected(OcrError{QStringLiteral("More than 1000 blocks detected. Select a smaller area.")});
        if (warnings && !failures.isEmpty())
            warnings->append(failures.join('\n') + QStringLiteral(" Kept native OCR for failed equation crops."));
        appendRecognitionEvidence(original, formulas);
        return original;
    }
    std::ranges::stable_sort(rows, {}, [](const Row &row) { return std::pair(row.box.top(), row.box.left()); });
    for (const auto &[language, baseline] : baselines) appendRecognitionEvidence(page, baseline);
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
        auto formulas = formulaReadings.recognize(formulaProvider, image, settings, stop, boxes,nullptr,formulaBudget);
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
            word.sourceBoxes = formulas->words[i].sourceBoxes;
            readIsolatedSymbol(word,symbolEvidence,image,settings,types[word.block-1] == "mixed",formulas->recognitionHistory);
        }
        appendRecognitionEvidence(page, *formulas);
    }
    page.rebuildHierarchy();
    for (int i = 0; i < types.size(); ++i)
        page.regions[i].type = types[i] == "math" ? QStringLiteral("equation") : QStringLiteral("text");
    return page;
}

std::expected<core::OcrPage, OcrError> recognizeSmartBlocks(const QImage &image, const OcrOptions &options,
    const QVector<core::ImageRegion> &regions, bool splitOnly, std::stop_token stop, bool includeUnmarked,
    const core::OcrPage *initialReading, QStringList *warnings, IOcrEngine *textProvider, IOcrEngine *formulaProvider,
    bool readBaseline, bool parallelFormulas)
{
    // Page OCR may borrow ink from adjacent text or unmarked margins. Selected
    // block OCR has a fixed output coordinate frame and stays within that block.
    auto crops = regions;
    bool adjusted = false;
    if (includeUnmarked && !splitOnly && std::ranges::any_of(regions,[](const auto &r) {
            return !r.notText && r.content == "math" && (!r.autoDetected || !r.detectedLabel.isEmpty());
        })) {
        crops = growFormulaBlocks(image,regions,stop);
        adjusted = crops.size() != regions.size();
        // The refinement command marks edited blocks as manual. During OCR
        // these are temporary input bounds; retain each original classification.
        for (auto &crop : crops) {
            const auto original = std::ranges::find(regions,crop.id,&core::ImageRegion::id);
            if (original == regions.end()) continue;
            const auto bounds = crop.bounds;
            adjusted |= bounds != original->bounds;
            crop = *original;
            crop.bounds = bounds;
        }
    }
    auto result = recognizeBlocks(image,options,crops,splitOnly,stop,includeUnmarked,initialReading,
                                  warnings,textProvider,formulaProvider,readBaseline,parallelFormulas);
    if (result && adjusted) {
        const core::RegionInk ink(image);
        auto outlines = regions;
        for (const auto &r : result->imageRegions) {
            auto saved = std::ranges::find(outlines,r.id,&core::ImageRegion::id);
            if (saved != outlines.end()) saved->content = r.content;
            else if (outlines.size() < 1000 && !std::ranges::any_of(regions,[&](const auto &old){return ink.shapeConflicts(old.shape(),r.shape());}))
                outlines.append(r);
        }
        result->imageRegions = std::move(outlines);
    }
    if (result && splitOnly) refineLineShapes(image,result->imageRegions,stop);
    return result;
}
}
