#include "symbolmatcher.h"

#include <QtEndian>
#include <bit>

namespace yagf::core {
namespace {
using Rows = std::array<quint16, 16>;

Rows decode(const QByteArray &mask)
{
    Rows rows;
    for (int y = 0; y < 16; ++y)
        rows[y] = qFromLittleEndian<quint16>(mask.constData() + 2 * y);
    return rows;
}

int inkArea(const Rows &rows)
{
    int area = 0;
    for (quint16 row : rows) area += std::popcount(row);
    return area;
}

// Include the eight neighbors of every ink cell. This keeps a detached dot
// or a second stroke from disappearing into an otherwise good overlap score.
Rows neighborhood(const Rows &rows)
{
    Rows result;
    for (int y = 0; y < 16; ++y) {
        quint16 band = rows[y];
        if (y > 0) band |= rows[y - 1];
        if (y < 15) band |= rows[y + 1];
        result[y] = quint16(band | (band << 1) | (band >> 1));
    }
    return result;
}

struct Alignment {
    Rows rows;
    Rows neighborhood;
};

QVector<Alignment> alignments(const Rows &rows, int area)
{
    QVector<Alignment> result;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            Rows shifted{};
            for (int y = 0; y < 16; ++y) {
                if (y + dy < 0 || y + dy >= 16) continue;
                shifted[y + dy] = dx < 0 ? rows[y] >> 1 : quint16(rows[y] << dx);
            }
            // A shift must preserve even small marks at the edge of the crop.
            if (inkArea(shifted) == area) result.append({shifted, neighborhood(shifted)});
        }
    }
    return result;
}

constexpr int minOverlapPercent = 90;

bool matches(const Alignment &query, const Rows &sample, const Rows &sampleNeighborhood, int totalInk)
{
    int overlap = 0;
    for (int y = 0; y < 16; ++y) {
        if ((query.rows[y] & ~sampleNeighborhood[y]) || (sample[y] & ~query.neighborhood[y]))
            return false;
        overlap += std::popcount(quint16(query.rows[y] & sample[y]));
    }
    return 100 * overlap >= minOverlapPercent * (totalInk - overlap);
}
} // namespace

void SymbolMatcher::insert(const QByteArray &mask)
{
    if (mask.size() != 32) return;
    const Rows rows = decode(mask);
    byInkArea.insert(inkArea(rows), {mask, rows, neighborhood(rows)});
}

QVector<QByteArray> SymbolMatcher::similar(const QByteArray &mask) const
{
    if (mask.size() != 32 || byInkArea.isEmpty()) return {};
    const Rows rows = decode(mask);
    const int area = inkArea(rows);
    if (!area) return {};
    const auto variants = alignments(rows, area);
    // Outside this interval even a perfect subset cannot meet the overlap threshold.
    const auto end = byInkArea.upperBound(100 * area / minOverlapPercent);
    QVector<QByteArray> keys;
    for (auto it = byInkArea.lowerBound((minOverlapPercent * area + 99) / 100); it != end; ++it) {
        const auto &sample = it.value();
        for (const auto &variant : variants) {
            if (matches(variant, sample.rows, sample.neighborhood, area + it.key())) {
                keys.append(sample.key);
                break;
            }
        }
    }
    return keys;
}

} // namespace yagf::core
