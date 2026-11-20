#pragma once
#include "iocrengine.h"
#include <QUrl>

namespace yagf::processing {
struct PageAreaAlternative {
    QString label;
    QString type;
    double score = 0;
};
struct PageArea {
    QString label;
    QString type;
    QRect bounds;
    double score = 0;
    QVector<PageAreaAlternative> alternatives;
    QString detectedType; // Retains the model's first choice when the user changes type.
};
struct PageLayout {
    QString model;
    QVector<PageArea> areas;
};
struct PageLayoutOptions {
    QUrl url = QUrl(QStringLiteral("http://127.0.0.1:8771"));
    QString token;
    double threshold = .5;
    int timeoutMs = 120000;
};
std::expected<PageLayout, OcrError> detectPageAreas(const QImage &image, const PageLayoutOptions &options,
                                                  std::stop_token stop = {});
// Inline formulas belong to their containing text block. Keep the paragraph intact
// and route it through mixed OCR rather than proposing overlapping tiny blocks.
void consolidateInlineAreas(QVector<PageArea> &areas);
QVector<core::ImageRegion> mergePageAreas(const QVector<core::ImageRegion> &existing,
                                         const QVector<PageArea> &accepted, bool replaceAutomatic);
}
