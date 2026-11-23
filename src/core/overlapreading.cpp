#include "overlapreading.h"

#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <tuple>

namespace yagf::core {
QRegion sharedReadingArea(const ImageRegion &block, const QVector<ImageRegion> &regions)
{
    const auto shape = block.shape();
    QRegion shared;
    for (const auto &other : regions)
        if (other.id != block.id && !other.notText && block.bounds.intersects(other.bounds))
            shared |= shape & other.shape();
    return shared;
}

// Place an unmatched reading beside the closest word on the same baseline.
// New lines retain their own hierarchy, including when an entire crop was empty.
void insertOverlapWord(QVector<OcrWord> &reading, OcrWord word)
{
    int closest = -1, distance = std::numeric_limits<int>::max(), maxLine = 0;
    int nearest = -1;
    qint64 nearestDistance = std::numeric_limits<qint64>::max();
    for (int i=0; i<reading.size(); ++i) {
        const auto &other = reading.at(i);
        maxLine = std::max(maxLine,other.line);
        const auto &box = other.bounds;
        const int dy = std::abs(box.center().y()-word.bounds.center().y());
        const int gap = std::max({0,box.left()-word.bounds.right(),word.bounds.left()-box.right()});
        const qint64 separation = qint64(gap)*gap + 4*qint64(dy)*dy;
        if (separation < nearestDistance) { nearest = i; nearestDistance = separation; }
        if (dy*2 > std::min(box.height(),word.bounds.height())) continue;
        const int dx = std::abs(box.center().x()-word.bounds.center().x());
        if (dx+2*dy < distance) { closest = i; distance = dx+2*dy; }
    }
    if (closest >= 0) {
        const auto &other = reading[closest];
        word.block = other.block; word.paragraph = other.paragraph; word.line = other.line;
        const int index = closest + (word.bounds.center().x() >= other.bounds.center().x());
        reading.insert(index,std::move(word));
    } else if (nearest >= 0) {
        const auto &anchor = reading.at(nearest);
        word.block = anchor.block;
        word.paragraph = anchor.paragraph;
        word.line = maxLine+1;
        const auto sameLine = [&](const OcrWord &other) {
            return other.block == anchor.block && other.paragraph == anchor.paragraph && other.line == anchor.line;
        };
        int index = nearest;
        if (word.bounds.center().y() < anchor.bounds.center().y()) {
            while (index > 0 && sameLine(reading.at(index-1))) --index;
        } else {
            while (index < reading.size() && sameLine(reading.at(index))) ++index;
        }
        reading.insert(index,std::move(word));
    } else {
        word.block = word.paragraph = word.line = 1;
        reading.append(std::move(word));
    }
}

bool touchesOcrCropEdge(const OcrWord &word, const QRegion &crop)
{
    if (word.geometryInferred || crop.isEmpty()) return false;
    const auto touches = [&](QRect box) {
        return !QRegion(box.adjusted(-1,-1,1,1)).subtracted(crop).isEmpty();
    };
    return word.sourceBoxes.isEmpty() ? touches(word.bounds) : std::ranges::any_of(word.sourceBoxes,touches);
}

namespace {
QRegion footprint(const OcrWord &word)
{
    if (word.sourceBoxes.isEmpty()) return word.bounds;
    QRegion result;
    for (const auto &box : word.sourceBoxes) result |= box;
    return result;
}

bool matchesVisibleHeight(QRect fragment, QRect whole, const QRegion &crop)
{
    if (crop.isEmpty()) return false;
    const QRect visible = (QRegion(whole) & crop).boundingRect();
    return !visible.isEmpty() && visible.height() < whole.height() &&
           (fragment & visible).height()*3 >= visible.height()*2;
}

bool sameLocation(const OcrWord &a, const OcrWord &b, const QRegion &shared,
                  const QRegion &cropA, const QRegion &cropB)
{
    const QRect both = a.bounds.intersected(b.bounds);
    if (both.isEmpty() || !shared.intersects(both)) return false;
    if (a.geometryInferred || b.geometryInferred) {
        // A paragraph-only engine localizes the crop, not its individual
        // words. Only compare whole items occupying essentially the same crop.
        const bool sameExtent = both.width()*10 >= 9*std::max(a.bounds.width(),b.bounds.width()) &&
                                both.height()*10 >= 9*std::max(a.bounds.height(),b.bounds.height());
        return sameExtent && !(footprint(a) & footprint(b) & shared).isEmpty() &&
               ((a.geometryInferred && b.geometryInferred && a.layoutBlock && b.layoutBlock) ||
                a.text.simplified() == b.text.simplified());
    }
    // A shared margin between two sloping lines is insufficient. A word
    // split by one engine may, however, cover only part of the other's width.
    const int height = std::min(a.bounds.height(),b.bounds.height());
    if (both.width()*3 < std::min(a.bounds.width(),b.bounds.width())*2 || both.height()*2 < height)
        return false;
    if (both.height()*3 < height*2) {
        // OCR boxes can extend below the actual ink on a severed line. Compare
        // against the part of the complete glyph visible inside that crop.
        const auto visibleMatch = [&](const OcrWord &fragment, const OcrWord &whole, const QRegion &crop) {
            return (touchesOcrCropEdge(fragment,crop) && matchesVisibleHeight(fragment.bounds,whole.bounds,crop)) ||
                   (fragment.atOcrCropEdge && matchesVisibleHeight(fragment.bounds,whole.bounds,shared));
        };
        if (!visibleMatch(a,b,cropA) && !visibleMatch(b,a,cropB)) return false;
    }
    return !(footprint(a) & footprint(b) & shared).isEmpty();
}

struct Span {
    QVector<int> indices;
    QRect bounds;
    bool reviewed = false;
    bool clipped = false;
    bool formula = false;
    double confidence = 0;
    int weight = 0;

    void add(int index, const OcrWord &word, const QRegion &crop) {
        indices.append(index);
        bounds |= word.bounds;
        reviewed |= word.reviewed;
        clipped |= word.atOcrCropEdge || touchesOcrCropEdge(word,crop);
        formula |= !word.latexText.isEmpty() || (word.layoutBlock && word.text.startsWith("\\("));
        const int width = word.bounds.width();
        confidence += width * word.confidence;
        weight += width;
    }
    auto rank(bool truncated) const {
        return std::tuple(reviewed,!truncated,!clipped,formula,confidence/std::max(1,weight));
    }
};
}

void reconcileOverlap(QVector<OcrWord> &primary, QVector<OcrWord> &incoming,
                      const QRegion &shared, const QRegion &primaryCrop, const QRegion &incomingCrop)
{
    if (shared.isEmpty() || primary.isEmpty() || incoming.isEmpty()) return;
    // Group cross-reading matches so a joined token competes with the
    // complete sequence of split tokens. Never compare words from one pass.
    const int offset = primary.size();
    QVector<int> parent(offset + incoming.size());
    std::iota(parent.begin(),parent.end(),0);
    const auto root = [&](int i) {
        while (i != parent[i]) { parent[i] = parent[parent[i]]; i = parent[i]; }
        return i;
    };
    QVector<int> candidates;
    int maxHeight = 0;
    for (int i=0; i<offset; ++i) {
        const auto &box = primary.at(i).bounds;
        if (shared.intersects(box)) {
            candidates.append(i);
            maxHeight = std::max(maxHeight,box.height());
        }
    }
    const auto top = [&](int i) { return primary.at(i).bounds.top(); };
    std::ranges::sort(candidates,{},top);
    QSet<int> matchedPrimary, matchedIncoming;
    for (int j=0; j<incoming.size(); ++j) {
        const auto &word = incoming.at(j);
        if (!shared.intersects(word.bounds)) continue;
        auto it = std::ranges::lower_bound(candidates,word.bounds.top()-maxHeight+1,{},top);
        for (; it != candidates.end() && top(*it) <= word.bounds.bottom(); ++it) {
            if (!sameLocation(primary.at(*it),word,shared,primaryCrop,incomingCrop)) continue;
            parent[root(offset+j)] = root(*it);
            matchedPrimary.insert(*it);
            matchedIncoming.insert(j);
        }
    }
    if (matchedPrimary.isEmpty()) return;
    struct Match { Span first, second; };
    QHash<int, Match> matches;
    for (int i=0; i<offset; ++i) if (matchedPrimary.contains(i))
        matches[root(i)].first.add(i,primary[i],primaryCrop);
    for (int j=0; j<incoming.size(); ++j) if (matchedIncoming.contains(j))
        matches[root(offset+j)].second.add(j,incoming[j],incomingCrop);

    QSet<int> removed, consumed;
    QHash<int,QVector<OcrWord>> replacements;
    for (const auto &match : matches) {
        const auto &a = match.first, &b = match.second;
        const QRect both = a.bounds.intersected(b.bounds);
        const bool comparable = both.width()*2 >= std::max(a.bounds.width(),b.bounds.width()) &&
                                both.height()*2 >= std::max(a.bounds.height(),b.bounds.height());
        // A tiny symbol inside a much larger item can be distinct. Resolve
        // unequal extents only when a crop edge explains the missing ink.
        const auto truncated = [&](const Span &small, const Span &large) {
            return small.clipped && large.bounds.adjusted(-1,-1,1,1).contains(small.bounds) &&
                   (large.bounds.width() > small.bounds.width()+2 || large.bounds.height() > small.bounds.height()+2);
        };
        const bool sameWidth = both.width()*2 >= std::max(a.bounds.width(),b.bounds.width());
        const auto cut = [&](const Span &fragment, const Span &whole, const QRegion &crop) {
            return truncated(fragment,whole) || (fragment.clipped && sameWidth &&
                (matchesVisibleHeight(fragment.bounds,whole.bounds,crop) ||
                 matchesVisibleHeight(fragment.bounds,whole.bounds,shared)));
        };
        const bool cutA = cut(a,b,primaryCrop), cutB = cut(b,a,incomingCrop);
        // A reader may box just one dot of an ellipsis while returning the
        // entire token. Exact agreement and containment identify that case.
        const bool sameToken = a.indices.size() == 1 && b.indices.size() == 1 &&
            (a.bounds.contains(b.bounds) || b.bounds.contains(a.bounds)) &&
            primary.at(a.indices.first()).text == incoming.at(b.indices.first()).text;
        if (!comparable && !cutA && !cutB && !sameToken) continue;
        for (int j : b.indices) consumed.insert(j);
        if (b.rank(cutB) <= a.rank(cutA)) continue;
        const auto &anchor = primary[a.indices.first()];
        auto &words = replacements[a.indices.first()];
        for (int j : b.indices) {
            auto word = incoming[j];
            word.block = anchor.block; word.paragraph = anchor.paragraph; word.line = anchor.line;
            words.append(std::move(word));
        }
        for (int i : a.indices) removed.insert(i);
    }
    QVector<OcrWord> kept;
    for (int i=0; i<primary.size(); ++i) {
        if (!removed.contains(i)) kept.append(std::move(primary[i]));
        else if (replacements.contains(i)) kept += replacements.value(i);
    }
    primary = std::move(kept);
    QVector<OcrWord> unmatched;
    for (int j=0; j<incoming.size(); ++j)
        if (!consumed.contains(j)) unmatched.append(std::move(incoming[j]));
    incoming = std::move(unmatched);
}
}
