#pragma once

#include <cmath>

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

    bool operator==(const PreprocessSettings &) const = default;
    [[nodiscard]] bool isIdentity() const
    {
        return std::abs(deskewDegrees) < 0.01 && !autoContrast && !denoise && quarterTurns == 0 && !adaptiveBinarization && bendPercent == 0 && !levelShadows;
    }
};

} // namespace yagf::core
