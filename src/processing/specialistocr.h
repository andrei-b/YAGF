#pragma once

#include "iocrengine.h"

namespace yagf::processing {

// Add review candidates to a primary OCR result. All boxes are crop-local.
QStringList collectSpecialistReadings(core::OcrPage &page, const QImage &image,
    const QVector<QRect> &formulaRegions, IOcrEngine *math, IOcrEngine *prose, const std::stop_token &stop, const OcrOptions &context = {});

} // namespace yagf::processing
