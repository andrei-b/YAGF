#pragma once

#include <cmath>
#include <QByteArray>
#include "coordinatetransform.h"

namespace yagf::core {

struct PreprocessSettings {
    double deskewDegrees = 0.0;
    bool autoContrast = false;
    bool denoise = false;
    int quarterTurns = 0; // Clockwise, applied before fine deskew.
    bool adaptiveBinarization = false;

    int inkThreshold = 0; // Local darkness threshold learned from pilot words; zero uses the original filter.

    static constexpr double maxBendPercent = 8.;
    double bendPercent = 0.; // Downward sag at the center as a percentage of upright page height.
    bool levelShadows = false;

    QByteArray restoredPng; // Full page before rotation and tonal preprocessing.
    CoordinatePipeline restorationGeometry; // Original raster -> restored raster, when geometry changed.
    QTransform perspective; // Upright normalized coordinates -> normalized output coordinates.

    bool operator==(const PreprocessSettings &) const = default;
    [[nodiscard]] bool isIdentity() const
    {
        return std::abs(deskewDegrees) < 0.01 && !autoContrast && !denoise && quarterTurns == 0 && !adaptiveBinarization && bendPercent == 0 && !levelShadows && restoredPng.isEmpty() && perspective.isIdentity();
    }
};

} // namespace yagf::core
