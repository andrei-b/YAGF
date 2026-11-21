#include "formulagrowth.h"

#include <QRegion>
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
} // namespace yagf::processing
