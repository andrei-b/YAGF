#include "textguidedanalysis.h"
#include "imageprocessor.h"
#include "tesseractengine.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <tuple>

namespace yagf::processing {
namespace {

double median(QVector<double> values)
{
    std::ranges::sort(values);
    return (values[(values.size() - 1) / 2] + values[values.size() / 2]) / 2;
}

// A median of widely separated word-pair slopes tolerates descenders and bad boxes.
// Each line contributes one vote, including lines from different columns.
std::optional<double> lineAngle(QVector<QRect> words)
{
    if (words.size() < 3) return {};
    std::ranges::sort(words, {}, &QRect::left);
    QVector<double> angles;
    const int stride = (words.size() + 31) / 32;
    for (int i = 0; i < words.size(); i += stride) {
        const auto &a = words[i];
        for (int j = i + stride; j < words.size(); j += stride) {
            const auto &b = words[j];
            const int dx = b.center().x() - a.center().x();
            if (dx < 5 * std::max(a.height(), b.height())) continue;
            angles.append(std::atan2(b.bottom() - a.bottom(), dx) * 180 / std::numbers::pi);
        }
    }
    if (angles.isEmpty()) return {};
    return median(angles);
}

} // namespace

std::expected<TextGuidance, OcrError> analyzeTextPilot(const QImage &pilot, const QString &language,
                                                     std::stop_token stop)
{
    auto page = TesseractEngine().recognize(pilot, {.language = language}, stop);
    if (!page) return std::unexpected(page.error());
    const auto reliable = [](const core::OcrWord &word) {
        return word.confidence >= 65 && word.text.size() >= 2;
    };
    double pilotSkew = 0;
    QTransform toSource;
    if (std::ranges::count_if(page->words, reliable) < 3 && !stop.stop_requested()) {
        const auto normalized = ImageProcessor::normalizedTextPilot(pilot);
        const auto estimate = ImageProcessor::assess(normalized);
        if (!estimate) return std::unexpected(OcrError{estimate.error()});
        const auto aligned = ImageProcessor::apply(normalized, {.deskewDegrees = estimate->skewDegrees});
        if (!aligned) return std::unexpected(OcrError{aligned.error()});
        auto retry = TesseractEngine().recognize(*aligned, {.language = language}, stop);
        if (!retry) return std::unexpected(retry.error());
        if (std::ranges::count_if(retry->words, reliable) > std::ranges::count_if(page->words, reliable)) {
            page = std::move(retry);
            pilotSkew = estimate->skewDegrees;
            toSource = QImage::trueMatrix(QTransform().rotate(-pilotSkew), pilot.width(), pilot.height()).inverted();
        }
    }
    std::map<std::tuple<int,int,int>, QVector<QRect>> lines;
    QVector<QRect> boxes;
    for (const auto &word : page->words) {
        if (!reliable(word)) continue;
        boxes.append(toSource.mapRect(word.bounds));
        lines[{word.block, word.paragraph, word.line}].append(word.bounds);
    }
    TextGuidance result;
    result.wordCount = boxes.size();
    QVector<double> angles;
    for (const auto &[id, words] : lines) {
        if (const auto angle = lineAngle(words)) angles.append(*angle);
    }
    if (angles.size() >= 2) {
        const double center = median(angles);
        QVector<double> inliers;
        for (double angle : angles)
            if (std::abs(angle - center) <= 1.0) inliers.append(angle);
        if (inliers.size() >= 2 && 3 * inliers.size() >= 2 * angles.size()) {
            const double skew = pilotSkew + median(inliers);
            if (std::abs(skew) <= 15) {
                result.skewDegrees = skew;
                result.lineCount = inliers.size();
            }
        }
    }
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    result.inkThreshold = ImageProcessor::estimateInkThreshold(pilot, boxes);
    return result;
}
}
