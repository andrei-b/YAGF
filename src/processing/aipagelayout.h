#pragma once
#include "iocrengine.h"
#include "regiongeometry.h"
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
    QString id; // Stable within one model response, including live score filtering.
    double splitParentScore = -1; // Used to rank proposals in the review dialog only.
};
struct PageLayout {
    QString model;
    QVector<PageArea> areas;
    QJsonObject rawOutput;
    QVector<PageArea> rawAreas; // Unconsolidated evidence for reapplying score thresholds.
    QVector<QRect> localInlineHints; // Geometry fallback for explicit crop subdivision.
};
struct PageLayoutOptions {
    QUrl url = QUrl(QStringLiteral("http://127.0.0.1:8771"));
    QString token;
    double threshold = .5;
    int timeoutMs = 120000;
};
std::expected<PageLayout, OcrError> detectPageAreas(const QImage &image, const PageLayoutOptions &options,
                                                  std::stop_token stop = {}, QJsonObject *evidence = nullptr);
// Route inline formulas through mixed OCR. Preserve bounded integrals as dedicated
// crops when the image supports partitioning their surrounding prose.
void consolidateInlineAreas(QVector<PageArea> &areas, const QImage &gray);
// Selected-block review can keep detected inline expressions as individual
// math crops and partition their containing paragraph around them.
void subdivideInlineAreas(QVector<PageArea> &areas, const QImage &gray, const QVector<QRect> &localHints);
QVector<core::ImageRegion> mergePageAreas(const QVector<core::ImageRegion> &existing,
                                         const QVector<PageArea> &accepted, bool replaceAutomatic,
                                         const core::RegionInk &ink = {});
}
