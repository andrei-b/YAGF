#pragma once
#include "ocrdocument.h"
#include <functional>

namespace yagf::core {
enum VerificationReason : unsigned {
    LowConfidence = 1, ConflictingReading = 2, UnknownWord = 4,
    GeometryAnomaly = 8, PendingCorrection = 16, UnrecognizedPage = 32, MathReading = 64
};
struct VerificationItem {
    int page = 1;
    QString wordId;
    QString text;
    QRect bounds;
    unsigned reasons = 0;
    bool pending = false;
};
struct VerificationQueue {
    QVector<VerificationItem> items;
    int recognizedPages = 0;
    int completedPages = 0; // Recognized pages with no outstanding queue items.
    qsizetype verifiedWords = 0;
    qsizetype totalWords = 0;
};
using SpellingCheck = std::function<bool(const OcrPage &, const OcrWord &, const QString &)>;
VerificationQueue buildVerificationQueue(const OcrDocument &document, const SpellingCheck &unknown = {}, double confidenceThreshold = 80);
QString verificationReasons(unsigned reasons);
}
