#pragma once

#include <cmath>

namespace yagf::core {

struct PreprocessSettings {
    double deskewDegrees = 0.0;
    bool autoContrast = false;
    bool denoise = false;

    bool operator==(const PreprocessSettings &) const = default;
    [[nodiscard]] bool isIdentity() const
    {
        return std::abs(deskewDegrees) < 0.01 && !autoContrast && !denoise;
    }
};

} // namespace yagf::core
