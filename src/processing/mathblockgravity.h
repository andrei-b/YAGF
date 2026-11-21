#pragma once
#include "ocrdocument.h"
#include <QImage>
#include <expected>
#include <stop_token>

namespace yagf::processing {
struct MathBlockRefinement {
    QVector<core::ImageRegion> regions;
    int movedComponents = 0;
    int changedBlocks = 0;
};
// An explicit image-only edit. Compare detached marks with actual formula/text
// ink, then move a shared boundary only if all remaining text ink fits safely.
std::expected<MathBlockRefinement, QString> refineMathBlocks(
    const QImage &image, const QVector<core::ImageRegion> &regions, std::stop_token stop = {});
}
