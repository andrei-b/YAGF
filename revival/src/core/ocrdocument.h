#pragma once

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
    bool bold = false;
    bool italic = false;
    int fontSizePt = 0;
    int sourceFontSizePt = 0;
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

struct OcrPage {
    QString id;
    int number = 1;
    int width = 0;
    int height = 0;
    QString engineId = QStringLiteral("tesseract");
    QString language;
    QVector<OcrWord> words;
    QVector<OcrRegion> regions;
    void rebuildHierarchy();
};

class OcrDocument {
public:
    QString sourcePath;
    QString engine = QStringLiteral("tesseract");
    QString language;
    int totalPages = 1;
    QVector<OcrPage> pages;

    static OcrDocument fromTsv(const QByteArray &tsv, int width, int height,
                               const QString &sourcePath, const QString &language, int pageNumber = 1);
    static bool fromJson(const QJsonObject &json, OcrDocument *document, QString *error);
    OcrPage *page(int number);
    const OcrPage *page(int number) const;
    void replacePage(OcrPage page);
    int wordCount() const;
    QString plainText() const;
    QString layoutHtml() const;
    QJsonObject toJson() const;
};

} // namespace yagf::core
