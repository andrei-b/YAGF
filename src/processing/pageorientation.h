#pragma once
#include "iocrengine.h"
namespace yagf::processing {
struct PageOrientation {
    int quarterTurns = 0; // Clockwise correction reported by Tesseract OSD.
    double confidence = 0;
};
// Optional Tesseract OSD; callers still support manual rotation without it.
std::expected<PageOrientation, OcrError> detectPageOrientation(const QImage &image, std::stop_token stop = {});
}
