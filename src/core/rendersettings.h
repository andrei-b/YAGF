#pragma once

namespace yagf::core {

constexpr bool validRenderDpi(int dpi) { return dpi == 150 || dpi == 200 || dpi == 300 || dpi == 400; }

struct RenderSettings {
    int dpi = 150;
    bool adaptive = false;
    bool operator==(const RenderSettings &) const = default;
};

} // namespace yagf::core
