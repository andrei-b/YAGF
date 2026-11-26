#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <expected>
#include <optional>
#include "coordinatetransform.h"

namespace yagf::core {

enum class TextGranularity { Word, Line, Paragraph, Block, Page };

// Coordinates, when present, are pixels in the recognized input image. An
// absent box means the model did not locate this item. order spans all kinds of
// region; parentId retains layout relationships without imposing editor IDs.
struct RecognitionRegion {
    QString id, parentId;
    std::optional<QRect> bounds;
    std::optional<QRect> inputArea; // Known submitted crop; never a model-produced box.
    int order = -1;
    double confidence = -1; // 0..100, or unknown.
};
enum class ScriptPosition { Subscript, Superscript };

// The box is relative to the word in unit coordinates so it also follows
// the word when a recognized crop is translated or resized.
struct ScriptGlyph {
    int start = 0, length = 0; // UTF-16 range in the original transcription.
    QRectF relativeBox;
    ScriptPosition position = ScriptPosition::Subscript;
    QRectF imageBox(const QRect &word) const;
};
void writeScriptGlyphs(QJsonObject &object, const QVector<ScriptGlyph> &glyphs);
bool readScriptGlyphs(const QJsonObject &object, const QString &text, QVector<ScriptGlyph> &glyphs);

struct TextRegion : RecognitionRegion {
    QString text;
    TextGranularity granularity = TextGranularity::Block;
    QString type = QStringLiteral("text");
    int block = 0, paragraph = 0, line = 0;
    QVector<ScriptGlyph> scripts;
};
struct FormulaRegion : RecognitionRegion {
    QString latex;
    bool display = true;
};
struct TableRegion : RecognitionRegion {
    QString text, markdown, html;
    // Original cell/span representation is retained rather than flattened into
    // fabricated word positions. The editor understands optional tableLayout.
    QJsonObject structure;
};
struct FigureRegion : RecognitionRegion {
    QString caption, type = QStringLiteral("figure");
};
struct RecognitionProvenance {
    QString model, task, language;
    int pageNumber = 1;
    QSize imageSize;
    // Placement of this input/crop in the document image; region boxes remain
    // relative to imageSize. No endpoint, authentication or machine identity.
    QRect inputArea;
    CoordinatePipeline sourceToInput; // Snapshot at recognition time; native region coordinates stay immutable.
};
struct RecognitionResult {
    QString engineId, modelVersion;
    QVector<TextRegion> text;
    QVector<FormulaRegion> formulas;
    QVector<TableRegion> tables;
    QVector<FigureRegion> figures;
    QString markdown;
    QJsonObject structuredOutput;
    RecognitionProvenance provenance;
};

QJsonObject recognitionResultJson(const RecognitionResult &result);
std::expected<RecognitionResult, QString> readRecognitionResult(const QJsonObject &json);
std::expected<void, QString> validateRecognitionResult(const RecognitionResult &result);

} // namespace yagf::core
