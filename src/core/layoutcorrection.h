#pragma once

#include "ocrdocument.h"

namespace yagf::core {

// Coordinates (including outline pieces) are absolute pixels in the supplied image.
QJsonArray layoutAnnotations(const QVector<ImageRegion> &regions);

// A split can be followed by several edits. Keep its original proposal and update
// the target until another split, a different image, or a different baseline.
std::expected<void, QString> recordLayoutCorrection(OcrPage &page, const QImage &image,
    const QVector<ImageRegion> &proposed, const QVector<ImageRegion> &corrected,
    const QString &source);

void beginLayoutSplit(OcrPage &page, QRect scope = {});
QJsonObject layoutTrainingJson(const LayoutTraining &training);
std::expected<LayoutTraining, QString> readLayoutTraining(const QJsonValue &value);

}
