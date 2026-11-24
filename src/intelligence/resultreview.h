#pragma once
#include "ocrdocument.h"
#include <QJsonArray>

namespace yagf::intelligence {
struct ReviewFragment {
    int word, offset;
    QString key, text;
};
struct ReviewChange {
    int word, offset;
    QString before, after, reason;
};
using ReviewBatch = QVector<ReviewFragment>;
QVector<ReviewBatch> reviewBatches(const core::OcrPage& page, QVector<int> words);
std::expected<QVector<ReviewChange>, QString> readChanges(const ReviewBatch& batch, QString response);
std::expected<core::OcrPage, QString> applyChanges(
    const core::OcrPage& page, QVector<ReviewChange> changes, const QString& model, bool unicodeMath);
}
