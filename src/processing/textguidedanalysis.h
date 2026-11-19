#pragma once

#include "iocrengine.h"
#include <optional>

namespace yagf::processing {

struct TextGuidance {
    std::optional<double> skewDegrees;
    int inkThreshold = 0;
    int wordCount = 0;
    int lineCount = 0;
};

// The pilot is upright (quarter turns already applied), before fine deskew or binarization.
std::expected<TextGuidance, OcrError> analyzeTextPilot(const QImage &pilot, const QString &language,
                                                     std::stop_token stop = {});
}
