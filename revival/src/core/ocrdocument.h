#pragma once

#include "preprocesssettings.h"

#include <QJsonObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

namespace yagf::core {

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
};

// User-drawn OCR areas exist before recognition and survive OCR page rebuilds.
struct ImageRegion {
    QString id;
    QRect bounds;
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
    QVector<OcrPage> pages;

    static bool fromJson(const QJsonObject &json, OcrDocument *document, QString *error);
    OcrPage *page(int number);
    const OcrPage *page(int number) const;
    void replacePage(OcrPage page);
    void removePage(int number);
    int wordCount() const;
    QString plainText() const;
    QString layoutHtml() const;
    QJsonObject toJson() const;
};

} // namespace yagf::core
