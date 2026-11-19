#include "imageprocessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace yagf::processing {
namespace {
constexpr int strips = 16;

double median(std::vector<double> values)
{
    std::ranges::sort(values);
    return (values[(values.size() - 1) / 2] + values[values.size() / 2]) / 2;
}

// Estimate the shift of the text rows between neighboring vertical strips.
// A constant shift comes from skew. A linear change in shift comes from a bow.
std::optional<double> bandBend(const QImage &mask, int top, int bottom)
{
    const int height = bottom - top;
    std::array<std::vector<double>, strips> profiles;
    std::array<bool, strips> usable{};
    for (int i = 0; i < strips; ++i) {
        const int left = i * mask.width() / strips, right = (i + 1) * mask.width() / strips;
        auto &profile = profiles[i];
        profile.resize(height);
        double sum = 0;
        for (int y = 0; y < height; ++y) {
            const uchar *row = mask.constScanLine(top + y);
            profile[y] = std::count(row + left, row + right, uchar{0});
            sum += profile[y];
        }
        const double mean = sum / height;
        int peaks = 0;
        for (int y = 1; y < height; ++y)
            if (profile[y] > mean && profile[y - 1] <= mean) ++peaks;
        usable[i] = peaks >= 3 && mean > .01 * (right - left) && mean < .45 * (right - left);
        for (auto &v : profile) v -= mean;
    }

    const int reach = std::min(height / 5, int(std::ceil(.3 * mask.width() / strips + .32 * mask.height() / strips)));
    std::vector<QPointF> shifts;
    for (int i = 1; i < strips; ++i) {
        if (!usable[i - 1] || !usable[i]) continue;
        std::vector<double> scores(2 * reach + 1);
        double best = .60;
        std::optional<int> offset;
        for (int dy = -reach; dy <= reach; ++dy) {
            double dot = 0, aa = 0, bb = 0;
            for (int y = reach; y < height - reach; ++y) {
                const double a = profiles[i - 1][y], b = profiles[i][y + dy];
                dot += a * b;
                aa += a * a;
                bb += b * b;
            }
            const double score = dot / std::sqrt(std::max(1., aa * bb));
            scores[dy + reach] = score;
            if (score > best) { best = score; offset = dy; }
        }
        if (offset && std::abs(*offset) < reach) {
            const int bin = *offset + reach;
            const double left = scores[bin - 1], right = scores[bin + 1];
            const double curvature = 2 * best - left - right;
            const double fraction = curvature > 1e-6 ? (right - left) / (2 * curvature) : 0.;
            shifts.emplace_back(i, *offset + fraction);
        }
    }
    if (shifts.size() < 6 || shifts.back().x() - shifts.front().x() < 6) return {};
    std::vector<double> slopes;
    for (auto a = shifts.begin(); a != shifts.end(); ++a)
        for (auto b = a + 1; b != shifts.end(); ++b)
            if (b->x() - a->x() >= 3) slopes.push_back((b->y() - a->y()) / (b->x() - a->x()));
    const double slope = median(slopes);
    std::vector<double> intercepts;
    for (auto point : shifts) intercepts.push_back(point.y() - slope * point.x());
    const double intercept = median(intercepts);
    for (auto &v : intercepts) v = std::abs(v - intercept);
    if (median(intercepts) > 1.5) return {};
    // For displacement 4*A*x*(1-x), its second difference is -8*A / strips².
    return -slope * strips * strips / 8;
}
} // namespace

std::optional<double> ImageProcessor::estimateBendPercent(const QImage &source)
{
    const auto pilot = source.width() > 960 || source.height() > 960
        ? source.scaled(960, 960, Qt::KeepAspectRatio, Qt::SmoothTransformation) : source;
    if (pilot.width() < 160 || pilot.height() < 120) return {};
    const auto mask = normalizedTextPilot(pilot);
    const auto upper = bandBend(mask, 0, mask.height() / 2);
    const auto lower = bandBend(mask, mask.height() / 2, mask.height());
    if (!upper || !lower || std::abs(*upper - *lower) > std::max(3., .012 * mask.height())) return {};
    const double percent = 50. * (*upper + *lower) / mask.height();
    if (std::abs(percent) > core::PreprocessSettings::maxBendPercent) return {};
    return std::abs(percent) < .6 ? 0. : percent;
}
} // namespace yagf::processing
