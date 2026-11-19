#pragma once

#include "preprocesssettings.h"

#include <QImage>
#include <QString>

#include <expected>
#include <optional>

namespace yagf::processing {

struct QualityAssessment {
    double skewDegrees = 0.0;
    int lowLuminance = 0;
    int highLuminance = 255;

    [[nodiscard]] bool needsContrast() const { return highLuminance - lowLuminance < 150; }
};

class ImageProcessor {
public:
    [[nodiscard]] static QImage normalizedTextPilot(const QImage &pilot);
    [[nodiscard]] static std::optional<double> estimateBendPercent(const QImage &upright);
    // Estimate ink/paper separation from confident word boxes on the same pilot image.
    [[nodiscard]] static int estimateInkThreshold(const QImage &pilot, const QVector<QRect> &words);
    // Inspect a 150-DPI pilot; small printed components request a higher render DPI.
    [[nodiscard]] static int recommendedRenderDpi(const QImage &pilot);
    [[nodiscard]] static std::expected<QualityAssessment, QString> assess(const QImage &source);
    [[nodiscard]] static std::expected<QImage, QString> apply(
        const QImage &source, const core::PreprocessSettings &settings);
};

} // namespace yagf::processing
