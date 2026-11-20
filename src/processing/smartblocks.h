#pragma once

#include "iocrengine.h"

namespace yagf::processing {

// A split-only pass returns editable image regions. Recognition also returns the
// words, running formula inference once for all isolated math spans on the page.
std::expected<core::OcrPage, OcrError> recognizeSmartBlocks(
    const QImage &image, const OcrOptions &options, const QVector<core::ImageRegion> &regions,
    bool splitOnly, std::stop_token stop, bool includeUnmarked = true,
    const core::OcrPage *initialReading = nullptr, QStringList *warnings = nullptr, IOcrEngine *textProvider = nullptr);

}
