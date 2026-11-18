#pragma once

#include "preprocesssettings.h"
#include "documentcontext.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

namespace yagf::core {

struct SpecialistReading {
    QString engine;
    QString text;
    QStringList sourceTokens;
    bool formula = false;
};

struct OcrWord {
    int page = 1;
    int block = 0;
    int paragraph = 0;
    int line = 0;
    QRect bounds;
    QString originalText;
    QString text;
    double confidence = -1;
    bool reviewed = false;
    QString id;
    QStringList sourceWordIds;
    bool bold = false;
    bool italic = false;
    int fontSizePt = 0;
    int sourceFontSizePt = 0;
    QVector<QRect> sourceBoxes; // Exact image fragments; empty for inserted text.
    bool layoutBlock = false; // A single OCR item can contain paragraphs, LaTeX or a table.
    QByteArray symbolShape; // 16 by 16 normalized ink mask for isolated OCR symbols.
    SpecialistReading alternative;
    QString readingEngine; // Specialist credited for a verbatim, accepted reading.
    QString fontFamily; // Closest installed face; empty keeps the editor/export default.
    int sourceFontSizePx = 0;
    [[nodiscard]] QString fontFamilyCss() const;
};

struct OcrLine {
    QString id;
    QRect bounds;
    QStringList wordIds;
};

struct OcrParagraph {
    QString id;
    QRect bounds;
    QVector<OcrLine> lines;
};

struct OcrRegion {
    QString id;
    QRect bounds;
    QString type = QStringLiteral("text");
    QVector<OcrParagraph> paragraphs;
    // Optional header and span metadata, aligned with the tab-separated block text.
    QJsonArray tableLayout;
    [[nodiscard]] bool isFigure() const;
    static bool validTableLayout(const QJsonValue &value);
};

// User-drawn OCR areas exist before recognition and survive OCR page rebuilds.
struct ImageRegion {
    QString id;
    QRect bounds;
    QString language; // Empty inherits the page recognition language.
    bool notText = false;
};

struct OcrPage {
    QString id;
    int number = 1;
    int width = 0;
    int height = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    PreprocessSettings preprocessing;
    QString engineId;
    QString language;
    QVector<OcrWord> words;
    QVector<OcrRegion> regions;
    QVector<ImageRegion> imageRegions;
    [[nodiscard]] QVector<int> specialistSpan(int start) const;
    void rebuildHierarchy();
    bool moveRegion(int index, int direction);
    bool extractRegion(const QVector<int> &wordIndices);
};

class OcrDocument {
public:
    QString sourcePath;
    QString engine;
    QString language;
    int totalPages = 1;
    DocumentContext context;
    bool preserveFormatting = false;
    QVector<OcrPage> pages;

    static bool fromJson(const QJsonObject &json, OcrDocument *result, QString *error);
    OcrPage *page(int number);
    [[nodiscard]] const OcrPage *page(int number) const;
    void replacePage(OcrPage replacement);
    void removePage(int number);
    [[nodiscard]] qsizetype wordCount() const;
    [[nodiscard]] QString plainText() const;
    [[nodiscard]] QString layoutHtml() const;
    [[nodiscard]] QJsonObject toJson() const;
};

} // namespace yagf::core
