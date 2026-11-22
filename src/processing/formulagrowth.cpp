#include "formulagrowth.h"
#include "compactassignment.h"
#include "formulatext.h"

#include <QRegion>
#include <QSet>
#include <QUuid>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace yagf::processing {
namespace {
using core::ImageRegion;

bool isFormula(const ImageRegion &region)
{
    return !region.notText && region.content == "math";
}

bool canDonate(const ImageRegion &region)
{
    return !region.notText && (region.content.isEmpty() || region.content == "text");
}

int gap(int a, int b, int c, int d)
{
    return std::max({0, a - d - 1, c - b - 1});
}

double distance(const QRect &a, const QRect &b)
{
    return std::hypot(gap(a.left(), a.right(), b.left(), b.right()),
                      gap(a.top(), a.bottom(), b.top(), b.bottom()));
}

struct Glyph {
    QRect box;
    int ink;
};

struct Neighborhood {
    QVector<Glyph> glyphs;
    int threshold;
};

Neighborhood components(const QImage &gray, const QRect &roi)
{
    const cv::Mat pixels(roi.height(), roi.width(), CV_8UC1,
        const_cast<uchar *>(gray.constScanLine(roi.top()) + roi.left()), gray.bytesPerLine());
    cv::Mat mask, labels, stats, centers;
    const auto otsu = cv::threshold(pixels, mask, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
    Neighborhood data{{}, std::clamp(qRound(otsu), 60, 200)};
    cv::threshold(pixels, mask, data.threshold, 255, cv::THRESH_BINARY_INV);
    const int count = cv::connectedComponentsWithStats(mask, labels, stats, centers);
    if (count > 3501) return data;
    for (int i = 1; i < count; ++i) {
        const int *s = stats.ptr<int>(i);
        data.glyphs.append({QRect(roi.x()+s[cv::CC_STAT_LEFT], roi.y()+s[cv::CC_STAT_TOP],
                                  s[cv::CC_STAT_WIDTH], s[cv::CC_STAT_HEIGHT]), s[cv::CC_STAT_AREA]});
    }
    return data;
}

// Look for aligned body letters or a base letter for an accent. Smaller
// adjoining pieces of a limit (e.g. i=1) can still form their own short run.
bool belongsToLine(const QRect &box, const QVector<Glyph> &glyphs, const QRect &formula,
                   double scale, const QVector<QRect> &fractionBars)
{
    // A numerator can itself resemble a short line of prose. Give neighbors
    // sharing a supported fraction bar a chance to attach as one expression.
    auto overBar = [scale](const QRect &glyph, const QRect &bar) {
        return glyph.left() >= bar.left()-1 && glyph.right() <= bar.right()+1 &&
            gap(glyph.top(),glyph.bottom(),bar.top(),bar.bottom()) < 0.55*scale;
    };
    int aligned = 0;
    for (const auto &glyph : glyphs) {
        const auto &other = glyph.box;
        if (other == box || formula.contains(other.center()) || other.height() < 0.65 * scale) continue;
        const int dx = gap(box.left(), box.right(), other.left(), other.right());
        if (dx > 1.6 * scale) continue;
        if (other.height() > 1.8 * box.height() && other.top() > box.bottom() &&
            other.top() - box.bottom() <= 0.36 * other.height() &&
            other.left()-1 <= box.center().x() && box.center().x() <= other.right()+1) return true;
        if (dx <= scale && std::abs(other.bottom()-box.bottom()) <= 0.23 * scale) {
            if (std::ranges::any_of(fractionBars, [&](const QRect &bar) {
                    return overBar(box,bar) && overBar(other,bar);
                })) continue;
            if (++aligned >= 2 || dx <= 0.32 * scale) return true;
        }
    }
    return false;
}

bool hasInk(const QImage &gray, const QRegion &area, int threshold)
{
    for (const auto &rect : area.intersected(gray.rect())) {
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            const auto *row = gray.constScanLine(y);
            if (std::any_of(row+rect.left(), row+rect.right()+1,
                            [threshold](uchar pixel) { return pixel <= threshold; })) return true;
        }
    }
    return false;
}

// Rectangular OCR crops require transferring a whole edge strip. Check even
// the part of that strip far outside the local symbol search before trimming.
QRect trimDonor(const QImage &gray, const QRect &donor, const QRect &grown, int threshold)
{
    const auto shared = donor.intersected(grown);
    QRect best;
    for (QRect kept : {QRect(donor.topLeft(), QPoint(donor.right(), shared.top()-1)),
                       QRect(QPoint(donor.left(), shared.bottom()+1), donor.bottomRight()),
                       QRect(donor.topLeft(), QPoint(shared.left()-1, donor.bottom())),
                       QRect(QPoint(shared.right()+1, donor.top()), donor.bottomRight())}) {
        if (kept.width() < 1 || kept.height() < 1 ||
            qint64(kept.width()) * kept.height() <= qint64(best.width()) * best.height()) continue;
        if (!hasInk(gray, QRegion(donor).subtracted(kept).subtracted(grown), threshold)) best = kept;
    }
    return best;
}

void growOne(const QImage &gray, QVector<ImageRegion> &blocks, int index, std::stop_token stop, bool largeOperator = false)
{
    const QRect original = blocks[index].bounds;
    if (!gray.rect().contains(original) || original.isEmpty()) return;
    const int margin = std::clamp(original.height() * 2, 16, 240);
    const QRect roi = original.adjusted(-margin,-margin,margin,margin).intersected(gray.rect());
    // Keep component labelling and the local proximity graph bounded on
    // accidental full-page selections and heavily speckled scans.
    if (qint64(roi.width()) * roi.height() > 8000000) return;
    const auto [glyphs, threshold] = components(gray, roi);
    if (glyphs.isEmpty() || stop.stop_requested()) return;

    QVector<int> seeds, heights, competitors;
    QVector<bool> allowed(glyphs.size(), false);
    for (int i = 0; i < glyphs.size(); ++i) {
        const auto &g = glyphs[i];
        if (original.contains(g.box) && g.ink >= 3) {
            seeds.append(i);
            if (g.box.height() >= 4 && g.box.width() < 5*g.box.height()) heights.append(g.box.height());
        } else {
            for (int b = 0; b < blocks.size(); ++b) {
                if (b != index && isFormula(blocks[b]) && blocks[b].bounds.contains(g.box.center())) {
                    competitors.append(i);
                    break;
                }
            }
        }
    }
    if (seeds.isEmpty() || heights.isEmpty()) return;
    auto mid = heights.begin()+heights.size()/2;
    std::nth_element(heights.begin(), mid, heights.end());
    const double scale = *mid;
    QVector<QRect> fractionBars;
    for (int seed : seeds) {
        const auto &bar = glyphs[seed].box;
        if (bar.width() < 1.25*scale || bar.width() < 5*bar.height() ||
            bar.height() > 0.3*scale) continue;
        // Require a body-sized glyph stacked with the rule. This rules out
        // the two strokes of a standalone equals sign.
        if (std::ranges::any_of(seeds, [&](int j) {
                const auto &g = glyphs[j].box;
                return g.height() >= 0.55*scale && g.center().x() >= bar.left() &&
                    g.center().x() <= bar.right() && (g.top() > bar.bottom() || g.bottom() < bar.top()) &&
                    distance(g,bar) <= 0.7*scale;
            })) fractionBars.append(bar);
    }
    const double minAnchorInk = std::max(2.0, scale*scale*0.008);
    QVector<double> rivalDistance(glyphs.size(), std::numeric_limits<double>::infinity());
    // Keep a halo of measured pixels around the search. A component touching
    // an internal ROI edge may continue beyond the labelled neighborhood.
    const QRect measured = roi.adjusted(roi.left() > 0, roi.top() > 0,
        -(roi.right() < gray.width()-1), -(roi.bottom() < gray.height()-1));
    const QRect search = original.adjusted(-qCeil(1.1*scale), -qCeil(1.25*scale),
                                           qCeil(1.1*scale), qCeil(1.25*scale)).intersected(measured);
    for (int i = 0; i < glyphs.size(); ++i) {
        if (stop.stop_requested()) return;
        const auto &g = glyphs[i];
        if (original.contains(g.box) || !search.contains(g.box) ||
            g.box.height() > 1.35*scale || g.box.width() > 1.6*scale) continue;
        const bool limit = largeOperator && g.box.height() <= 0.65 * original.height() &&
            (g.box.center().y() < original.top() || g.box.center().y() > original.bottom()) &&
            gap(g.box.left(),g.box.right(),original.left(),original.right()) <= 0.35 * original.height();
        if (std::ranges::any_of(blocks, [&](const auto &b) {
                return &b != &blocks.at(index) && !canDonate(b) && b.bounds.intersects(g.box);
            }) || (!limit && belongsToLine(g.box, glyphs, original, scale, fractionBars))) continue;
        for (int j : competitors) rivalDistance[i] = std::min(rivalDistance[i],distance(glyphs[j].box,g.box)/scale);
        allowed[i] = true;
    }

    // Greedily attach the cheapest reachable glyph. Aligned scripts get a
    // lower cost; each hop spends some of the original seed's distance budget.
    using Edge = std::pair<double,int>;
    std::priority_queue<Edge,std::vector<Edge>,std::greater<Edge>> frontier;
    QVector<double> cost(glyphs.size(), std::numeric_limits<double>::infinity());
    QVector<bool> claimed(glyphs.size(), false);
    auto offer = [&](int anchor, double previous) {
        const auto &a = glyphs[anchor].box;
        for (int i = 0; i < glyphs.size(); ++i) {
            if (!allowed[i] || claimed[i]) continue;
            const auto &b = glyphs[i].box;
            const double d = distance(a,b) / scale;
            // A detached dot or broken stroke may follow a nearby glyph,
            // but it cannot bridge the gap to another symbol.
            if (glyphs[i].ink < minAnchorInk && d > 0.18) continue;
            const bool stacked = gap(a.left(),a.right(),b.left(),b.right()) <= 0.18*scale;
            const double score = previous + d + (stacked ? 0.10 : 0.24);
            if (d > 0.80 || score > 1.35 || score >= cost[i]) continue;
            // Ties between adjacent equations are too ambiguous to transfer.
            if (rivalDistance[i] <= score + 0.08) continue;
            cost[i] = score;
            frontier.emplace(score,i);
        }
    };
    for (int seed : seeds) {
        if (stop.stop_requested()) return;
        offer(seed,0);
    }
    QVector<int> ordered;
    while (!frontier.empty() && !stop.stop_requested()) {
        auto [score,i] = frontier.top();
        frontier.pop();
        if (claimed[i]) continue;
        claimed[i] = true;
        ordered.append(i);
        if (glyphs[i].ink >= minAnchorInk) offer(i,score);
    }
    if (ordered.isEmpty() || stop.stop_requested()) return;

    auto apply = [&](const QRect &grown) {
        if (grown == blocks[index].bounds) return true;
        for (int i = 0; i < glyphs.size(); ++i) {
            const auto &box = glyphs[i].box;
            if (grown.intersected(box) != original.intersected(box) &&
                (!claimed[i] || !grown.contains(box))) return false;
        }
        QVector<std::pair<int,QRect>> trims;
        for (int b = 0; b < blocks.size(); ++b) {
            if (b == index || !blocks[b].bounds.intersects(grown)) continue;
            if (!canDonate(blocks[b]) || original.intersects(blocks[b].bounds)) return false;
            if (grown.contains(blocks[b].bounds) && std::ranges::any_of(ordered, [&](int i) {
                    return blocks[b].bounds.contains(glyphs[i].box.center());
                })) {
                trims.append({b,QRect{}}); // Every glyph in this little block moved to the formula.
                continue;
            }
            const auto kept = trimDonor(gray,blocks[b].bounds,grown,threshold);
            if (kept.isEmpty()) return false;
            trims.append({b,kept});
        }
        auto resize = [&](int b, const QRect &box) {
            blocks[b].bounds = box;
            blocks[b].autoDetected = false;
            blocks[b].detectedLabel.clear();
            blocks[b].detectionScore = -1;
        };
        for (const auto &[b,kept] : trims) resize(b,kept);
        resize(index,grown);
        return true;
    };
    QRect combined = original;
    for (int i : ordered) combined |= glyphs[i].box;
    if (!apply(combined)) {
        for (int i : ordered) {
            if (stop.stop_requested()) return;
            apply(blocks[index].bounds.united(glyphs[i].box));
        }
    }
}
} // namespace

QVector<QRect> boundedIntegralSigns(const QImage &gray, QRect candidate)
{
    if (gray.format() != QImage::Format_Grayscale8 || candidate.height() < 8) return {};
    const int halo = std::clamp(2 * candidate.height(), 24, 160);
    const int side = std::max(8,halo/4);
    const QRect roi = candidate.adjusted(-side, -halo, side, halo).intersected(gray.rect());
    if (roi.isEmpty() || qint64(roi.width()) * roi.height() > 1000000) return {};
    const auto [glyphs, threshold] = components(gray, roi);
    QVector<QRect> signs;
    for (const auto &glyph : glyphs) {
        const QRect op = glyph.box;
        const int h = op.height(), w = op.width();
        if (!candidate.contains(op.center()) || h < 16 || w < 4 || h < 2*w || h > 9*w) continue;
        // An integral has opposing hooks. Compare each end to the nearby
        // stem so this also works with slanted type and residual page skew.
        const auto inkSpan = [&](int first, int last) {
            int left = w, right = -1;
            for (int y = first; y <= last; ++y) {
                const auto *row = gray.constScanLine(op.y()+y) + op.x();
                for (int x = 0; x < w; ++x) if (row[x] <= threshold) {
                    left = std::min(left,x); right = std::max(right,x);
                }
            }
            return std::pair(left,right);
        };
        const auto top = inkSpan(0,h/5), upperStem = inkSpan(h/4,h/3);
        const auto bottom = inkSpan(h-1-h/5,h-1), lowerStem = inkSpan(2*h/3,3*h/4);
        const int hook = std::max(2,w/4);
        if (top.second-upperStem.second < hook || lowerStem.first-bottom.first < hook ||
            upperStem.second-upperStem.first+1 > .65*w || lowerStem.second-lowerStem.first+1 > .65*w) continue;
        QRect upper, lower;
        for (const auto &piece : glyphs) {
            const QRect b = piece.box;
            if (piece.ink < 3 || b.height() < 2 || b.height() > .55*h || b.width() > .85*h ||
                std::abs(b.center().x()-op.center().x()) > .48*h ||
                gap(b.left(),b.right(),op.left(),op.right()) > .22*h) continue;
            const bool above = b.bottom() < op.top() && op.top()-b.bottom() <= .42*h;
            const bool below = op.bottom() < b.top() && b.top()-op.bottom() <= .42*h;
            if ((!above && !below) || belongsToLine(b,glyphs,op,.45*h,{})) continue;
            if (above) upper |= b;
            else lower |= b;
        }
        if (upper.isValid() && lower.isValid()) signs.append(op | upper | lower);
    }
    return signs;
}

QRect growSymbolCrop(const QImage &gray, QRect seed, QRect window,
    const QVector<QRect> &protectedBoxes, bool largeOperator, std::stop_token stop)
{
    const int halo = std::clamp(seed.height()*2,16,240);
    const QRect roi = seed.adjusted(-halo,-halo,halo,halo).intersected(window).intersected(gray.rect());
    if (!roi.contains(seed)) return seed;
    QVector<ImageRegion> boxes{{"seed",seed.translated(-roi.topLeft()),{},false,"math"}};
    for (const auto &box : protectedBoxes) if (box.intersects(roi))
        boxes.append({{},box.translated(-roi.topLeft()),{},true,"picture"});
    growOne(gray.copy(roi),boxes,0,stop,largeOperator);
    return boxes[0].bounds.translated(roi.topLeft());
}

QVector<core::ImageRegion> growFormulaBlocks(const QImage &image,
    const QVector<core::ImageRegion> &blocks, std::stop_token stop)
{
    auto result = blocks;
    const auto gray = image.convertToFormat(QImage::Format_Grayscale8);
    for (int i = 0; i < blocks.size() && !stop.stop_requested(); ++i)
        if (isFormula(blocks[i])) growOne(gray,result,i,stop);
    if (stop.stop_requested()) return blocks;
    for (qsizetype i = result.size(); i-- > 0;)
        if (result[i].bounds.isEmpty() && !blocks[i].bounds.isEmpty()) result.removeAt(i);
    return result;
}
// Estimate expression extents from fraction stacks and paired equals strokes.
// Tight groups of letters on either side are prose boundaries. Short,
// separated groups can be variables, signs, powers or a second fraction.
QVector<QRect> findInlineFormulaHints(const QImage &gray, QRect parent, std::stop_token stop)
{
    if (gray.format() != QImage::Format_Grayscale8 || !gray.rect().contains(parent) ||
        parent.isEmpty() || qint64(parent.width())*parent.height() > 8000000) return {};
    const auto data = components(gray, parent);
    QVector<int> heights;
    for (const auto &g : data.glyphs)
        if (g.box.height() >= 4 && g.box.width() <= g.box.height()*3) heights.append(g.box.height());
    if (heights.size() < 4) return {};
    auto middle = heights.begin()+heights.size()/2;
    std::nth_element(heights.begin(), middle, heights.end());
    const int em = *middle;
    QVector<QRect> hints;
    for (const auto &g : data.glyphs) {
        if (stop.stop_requested()) return {};
        QRect bar = g.box;
        if (bar.width() < std::max(6, em) || bar.width() > 12*em ||
            bar.height()*4 > bar.width() || bar.height() > std::max(2, em/3) ||
            g.ink < .55*bar.width()*bar.height()) continue;
        QRect above, below, equals;
        for (const auto &other : data.glyphs) {
            const QRect b = other.box;
            if (b == bar || b.left() < bar.left()-1 || b.right() > bar.right()+1) continue;
            const int dy = gap(bar.top(),bar.bottom(),b.top(),b.bottom());
            if (dy > .65*em || b.intersects(bar)) continue;
            if (b.top() > bar.bottom() && dy <= em/2 && std::abs(b.width()-bar.width()) <= bar.width()/6 &&
                b.height() <= bar.height()+1 && b.height()*4 <= b.width()) equals = b;
            if (b.height() < .45*em || b.height() > 1.8*em) continue;
            if (b.bottom() < bar.top()) above |= b;
            else below |= b;
        }
        QRect seed;
        if (!above.isEmpty() && !below.isEmpty()) seed = bar | above | below;
        else if (!equals.isEmpty()) seed = bar | equals;
        else continue;

        // Group glyphs by horizontal gaps at the operator's baseline. Joined
        // numerator/denominator projections are already part of the seed.
        QVector<QRect> glyphs;
        const int mid = bar.center().y();
        for (const auto &other : data.glyphs) {
            const auto b = other.box;
            if (b.bottom() < mid-em/2 || b.top() > mid+em/2 ||
                gap(b.left(),b.right(),bar.left(),bar.right()) > 10*em) continue;
            glyphs.append(b);
        }
        std::ranges::sort(glyphs, {}, &QRect::left);
        QVector<QRect> groups;
        for (const auto &b : glyphs) {
            if (groups.isEmpty() || b.left()-groups.last().right()-1 > std::max(1, em/4)) groups.append(b);
            else groups.last() |= b;
        }
        const auto extend = [&](const QRect &group) {
            if ((group.width() < .55*em && group.height() < .45*em) ||
                gap(seed.left(),seed.right(),group.left(),group.right()) > em ||
                (group.width() > 1.65*em && !group.intersects(bar))) return false;
            seed |= group;
            return true;
        };
        for (auto it = groups.crbegin(); it != groups.crend(); ++it)
            if (it->left() <= bar.right() && !extend(*it)) break;
        for (const auto &group : groups)
            if (group.right() >= bar.left() && !extend(group)) break;
        // Equals without an operand could be two short parallel rules.
        if (above.isEmpty() && seed.width() <= bar.width()+2) continue;
        for (qsizetype i = hints.size(); i-- > 0;)
            if (hints[i].intersects(seed)) seed |= hints.takeAt(i);
        hints.append(seed);
    }
    return hints;
}

QRect inkBox(const QImage &gray, QRect area)
{
    if (!area.isValid() || gray.format() != QImage::Format_Grayscale8) return {};
    area = area.intersected(gray.rect());
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

// Snap one formula to clear gutters without cutting through a neighboring line.
static QRect inlineFormulaCut(const QImage &gray, QRect parent, QRect hint, QVector<qint8> &rowInk)
{
    const QRect seed = inkBox(gray,hint);
    if (seed.isEmpty()) return {};
    const auto rowHasInk = [&](int y) {
        auto &ink = rowInk[y-parent.top()];
        if (ink < 0) {
            const auto *row = gray.constScanLine(y);
            ink = std::any_of(row+parent.left(), row+parent.right()+1, [](uchar value){return value < 175;});
        }
        return ink != 0;
    };
    QRect cut = seed;
    const int verticalLimit = seed.height()+4;
    while (cut.top() > parent.top() && rowHasInk(cut.top()-1)) {
        if (seed.top()-cut.top() >= verticalLimit) return {};
        cut.adjust(0,-1,0,0);
    }
    while (cut.bottom() < parent.bottom() && rowHasInk(cut.bottom()+1)) {
        if (cut.bottom()-seed.bottom() >= verticalLimit) return {};
        cut.adjust(0,0,0,1);
    }
    const auto columnHasInk = [&](int x) {
        for (int y = cut.top(); y <= cut.bottom(); ++y)
            if (gray.constScanLine(y)[x] < 175) return true;
        return false;
    };
    while (cut.left() > parent.left() && columnHasInk(cut.left()-1)) {
        if (seed.left()-cut.left() >= seed.height()) return {};
        cut.adjust(-1,0,0,0);
    }
    while (cut.right() < parent.right() && columnHasInk(cut.right()+1)) {
        if (cut.right()-seed.right() >= seed.height()) return {};
        cut.adjust(0,0,1,0);
    }
    return cut;
}

QVector<InlinePart> partitionInlineFormulas(const QImage &gray, QRect parent, const QVector<QRect> &hints)
{
    if (gray.format() != QImage::Format_Grayscale8 || !gray.rect().contains(parent) ||
        parent.isEmpty() || hints.isEmpty()) return {};
    // Formula hints on the same line share the row projections.
    QVector<qint8> rowInk(parent.height(), -1);
    QVector<InlinePart> cuts;
    QSet<int> accepted;
    for (int i = 0; i < hints.size(); ++i) {
        if (!parent.contains(hints[i])) continue;
        const QRect cut = inlineFormulaCut(gray,parent,hints[i],rowInk);
        if (cut.isEmpty()) continue;
        const bool conflict = std::ranges::any_of(cuts,[&](const InlinePart &p) {
            const bool sameBand = p.bounds.top() == cut.top() && p.bounds.bottom() == cut.bottom();
            return sameBand ? p.bounds.intersects(cut)
                            : p.bounds.top() <= cut.bottom() && cut.top() <= p.bounds.bottom();
        });
        if (conflict) continue;
        cuts.append({cut,i});
        accepted.insert(i);
    }
    if (cuts.isEmpty()) return {};
    std::ranges::sort(cuts, {}, [](const InlinePart &p){return std::pair(p.bounds.top(),p.bounds.left());});
    QVector<InlinePart> result;
    const auto append = [&](QRect cell, int formula = -1) {
        if (cell.isEmpty()) return;
        const auto ink = inkBox(gray,cell);
        if (!ink.isEmpty()) result.append({ink.adjusted(-1,-1,1,1).intersected(cell),formula});
    };
    int top = parent.top();
    bool besideText = false;
    for (int i = 0; i < cuts.size();) {
        const QRect band = cuts[i].bounds;
        append({parent.left(),top,parent.width(),band.top()-top});
        int left = parent.left();
        const auto appendBeside = [&](int end) {
            const auto before = result.size();
            append({left,band.top(),end-left,band.height()});
            besideText |= result.size() != before;
        };
        do {
            const auto &piece = cuts[i++];
            appendBeside(piece.bounds.left());
            append(piece.bounds,piece.formula);
            left = piece.bounds.right()+1;
        } while (i < cuts.size() && cuts[i].bounds.top()==band.top());
        appendBeside(parent.right()+1);
        top = band.bottom()+1;
    }
    if (!besideText) return {};
    append({parent.left(),top,parent.width(),parent.bottom()-top+1});
    for (int i = 0; i < hints.size(); ++i) {
        if (accepted.contains(i) || !parent.contains(hints[i])) continue;
        for (auto &part : result)
            if (part.formula < 0 && part.bounds.intersects(hints[i])) part.containsUnsplitFormula = true;
    }
    return result;
}

QVector<core::ImageRegion> refineFormulaRegions(const QImage &image,
    const QVector<core::ImageRegion> &regions, const QVector<core::OcrWord> &words, std::stop_token stop)
{
    QVector<QRect> wordHints;
    for (const auto &word : words) {
        if (word.geometryInferred || word.bounds.isEmpty()) continue;
        const auto text = core::formulaSourceText(word).trimmed();
        const auto spans = core::mathTextSpans(text);
        if ((spans.size() == 1 && spans[0].length == text.size()) || compactAssignment(text))
            wordHints.append(word.bounds);
    }
    const auto gray = image.convertToFormat(QImage::Format_Grayscale8);
    const auto grown = growFormulaBlocks(gray,regions,stop);
    QVector<core::ImageRegion> result;
    for (int i = 0; i < grown.size(); ++i) {
        if (stop.stop_requested()) return regions;
        const auto &parent = grown[i];
        QVector<InlinePart> parts;
        if (!parent.notText && (parent.content.isEmpty() || parent.content=="text" || parent.content=="mixed")) {
            auto hints = findInlineFormulaHints(gray,parent.bounds,stop);
            for (const auto &box : wordHints)
                if (parent.bounds.contains(box) && std::ranges::none_of(hints,[&](QRect hint){return hint.contains(box);}))
                    hints.append(box);
            parts = partitionInlineFormulas(gray,parent.bounds,hints);
        }
        if (parts.isEmpty() || result.size()+parts.size()+grown.size()-i-1 > 1000) {
            result.append(parent);
            continue;
        }
        for (const auto &part : parts) {
            core::ImageRegion child;
            child.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            child.bounds = part.bounds;
            child.content = part.content();
            if (part.formula >= 0) child.detectedLabel = "inline_formula_geometry";
            child.language = parent.language;
            result.append(std::move(child));
        }
    }
    return stop.stop_requested() ? regions : result;
}
} // namespace yagf::processing
