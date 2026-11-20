#pragma once
#include "ocrdocument.h"
#include <expected>

namespace yagf::core {
QJsonArray ocrReadingsJson(const QVector<OcrReading> &readings);
std::expected<QVector<OcrReading>, QString> readOcrReadings(const QJsonValue &json, QSize size);
QString ocrReadingText(const OcrReading &reading);
QVector<QVector<int>> ocrReadingSpans(const OcrPage &page);
QString matchedOcrReading(const OcrPage &page, const QVector<int> &span, const OcrReading &reading);
std::expected<OcrPage, QString> acceptOcrReading(const OcrPage &page, const QVector<int> &span,
                                               const OcrReading &reading, const QString &text);
void translateOcrReadings(QVector<OcrReading> &readings, QPoint offset);
QVector<OcrReading> cropOcrReadings(const QVector<OcrReading> &readings, QRect area);
void mergeOcrReadings(QVector<OcrReading> &target, QVector<OcrReading> incoming, QRect area);
}
