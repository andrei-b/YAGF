#pragma once
#include "recognitionresult.h"

namespace yagf::core {
// Project-level codec. Standalone RecognitionResult JSON stays self-contained;
// a page shares large deformation fields between its native OCR readings.
class CoordinateFrames {
    QHash<QString,CoordinatePipeline> frames;
public:
    QJsonObject encode(const RecognitionResult &result);
    std::expected<RecognitionResult,QString> decode(const QJsonObject &json) const;
    QJsonObject toJson() const;
    std::expected<void,QString> load(const QJsonObject &json);
};
}
