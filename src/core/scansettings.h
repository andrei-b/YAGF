#pragma once

#include <QString>

namespace yagf::core {

struct ScanSettings {
    static constexpr int minimumDpi = 75;
    static constexpr int maximumDpi = 1200;
    QString device;
    int dpi = 300;
    QString mode; // An empty mode lets the scanner choose.
    bool operator==(const ScanSettings &) const = default;
};

} // namespace yagf::core
