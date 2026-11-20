#pragma once

#include "iocrengine.h"

namespace yagf::processing {
// Dictionary misses are candidates, not proof of mathematics. Keep native OCR
// and attach math readings for source-linked review and explicit acceptance.
std::expected<QStringList, OcrError> collectSpellingFormulaReadings(
    core::OcrPage &page, const QImage &image, std::stop_token stop);
}
