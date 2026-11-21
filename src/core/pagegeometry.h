#pragma once
#include "ocrdocument.h"

namespace yagf::core {
std::expected<core::OcrPage,QString> remapPageGeometry(
    const core::OcrPage &page,core::CoordinatePipeline destination);
void attachRecognitionCoordinates(core::OcrPage &page);
// Map model-local geometry through its immutable recognition-time frame.
std::expected<QPolygonF,QString> recognitionOutlineOnPage(
    const core::RecognitionResult &reading,const core::RecognitionRegion &region,
    const core::CoordinatePipeline &current);
}
