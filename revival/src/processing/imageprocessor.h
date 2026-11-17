#pragma once

#include "preprocesssettings.h"

#include <QImage>
#include <QString>

#include <expected>

namespace yagf::processing {

struct QualityAssessment {
    double skewDegrees = 0.0;
    int lowLuminance = 0;
    int highLuminance = 255;

    [[nodiscard]] bool needsContrast() const { return highLuminance - lowLuminance < 150; }
};

class ImageProcessor {
public:
    [[nodiscard]] static std::expected<QualityAssessment, QString> assess(const QImage &source);
    [[nodiscard]] static std::expected<QImage, QString> apply(
        const QImage &source, const core::PreprocessSettings &settings);
};

} // namespace yagf::processing
