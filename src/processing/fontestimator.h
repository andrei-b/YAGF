#pragma once

#include "ocrdocument.h"

#include <QImage>
#include <atomic>
#include <stop_token>

namespace yagf::processing {

// Compare recognized strings with local font rasters; never changes text or boxes.
// Layout engines that return a passage as one item receive a dominant block style.
void estimateTextFormatting(core::OcrPage &page, const QImage &image, std::stop_token stop = {},
                            const std::atomic_bool *cancelled = nullptr);

} // namespace yagf::processing
