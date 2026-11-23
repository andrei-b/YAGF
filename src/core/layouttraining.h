#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QRect>
#include <QString>
#include <QVector>

namespace yagf::core {
struct LayoutExample {
    QString id, splitId, source, imageHash;
    QByteArray png;
    QRect scope; // Page pixels. A selected-block split can cover part of the page.
    QJsonArray proposed, corrected;
    qint64 imageCacheKey = 0; // Process-local fast path for repeated edits on the displayed image.
};
struct LayoutTraining {
    QString splitId;
    QRect scope;
    QVector<LayoutExample> examples;
};
}
