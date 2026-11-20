#pragma once
#include "ocrdocument.h"
#include <stop_token>

namespace yagf::core {
// Apply one dominant point size and native image em size to newly recognized
// prose in each block. Explicit image blocks take precedence over OCR block IDs.
// A region-only recognition is one block even if its provider subdivides it.
// Does not alter text, geometry, face, emphasis, formulas, or explicit size edits.
void normalizeBlockFontSizes(OcrPage &page, const QVector<ImageRegion> &blocks = {},
                             bool singleBlock = false, std::stop_token stop = {});
}
