#pragma once

#include <QJsonObject>
#include <QRect>
#include <QString>
#include <QVector>

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
};

class OcrDocument {
public:
    QString sourcePath;
    QString engine = QStringLiteral("tesseract");
    QString language;
    int width = 0;
    int height = 0;
    QVector<OcrWord> words;

    static OcrDocument fromTsv(const QByteArray &tsv, int width, int height,
                               const QString &sourcePath, const QString &language);
    static bool fromJson(const QJsonObject &json, OcrDocument *document, QString *error);
    QString plainText() const;
    QJsonObject toJson() const;
};
