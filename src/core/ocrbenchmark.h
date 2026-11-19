#pragma once
#include "ocrdocument.h"
#include <expected>
#include <stop_token>

namespace yagf::core {
struct TextErrorRate {
    qint64 errors = 0;
    qint64 referenceUnits = 0;
    QJsonObject toJson() const;
};
struct BenchmarkScores {
    TextErrorRate characters, words;
    QJsonObject geometry;
    QJsonObject toJson() const;
};
QString benchmarkReferenceProblem(const OcrPage &page);
QString benchmarkText(const OcrPage &page);
// NFC, whitespace collapsed, case/punctuation preserved; characters are Unicode code points.
std::expected<BenchmarkScores, QString> scoreRecognition(const OcrPage &reference, const OcrPage &reading,
                                                        bool compareWordGeometry, std::stop_token stop = {});
}
