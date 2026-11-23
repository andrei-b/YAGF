#pragma once

#include "ocrdocument.h"

namespace yagf::core {
// Compare independent readings in page coordinates. Matching spans stay at
// their position in the primary reading. Unmatched incoming words remain in
// incoming so the caller can place them using its normal reading order.
void reconcileOverlap(QVector<OcrWord> &primary, QVector<OcrWord> &incoming,
                      const QRegion &shared, const QRegion &primaryCrop, const QRegion &incomingCrop);
bool touchesOcrCropEdge(const OcrWord &word, const QRegion &crop);
void insertOverlapWord(QVector<OcrWord> &reading, OcrWord word);
QRegion sharedReadingArea(const ImageRegion &block, const QVector<ImageRegion> &regions);
}
