#pragma once

#include <QHash>
#include <QJsonObject>
#include <QPolygonF>
#include <QSizeF>
#include <QTransform>
#include <QVector>
#include <expected>

namespace yagf::core {

// Continuous pixel-edge coordinates: an image occupies [0,width] x [0,height].
// A displacement grid is regular in the input space, with bilinear interpolation.
struct CoordinateStep {
    QString operation;
    QSizeF inputSize, outputSize;
    QTransform projective;
    QSize gridSize;
    QVector<QPointF> displacement;
    bool operator==(const CoordinateStep &) const = default;
};

struct CoordinatePipeline {
    QString sourceUnit = QStringLiteral("pixels");
    QString sourceFrame = QStringLiteral("original"); // Legacy restored images may have no known map to the original.
    QSizeF sourceSize;
    QVector<CoordinateStep> steps;

    [[nodiscard]] QSizeF outputSize() const { return steps.isEmpty() ? sourceSize : steps.last().outputSize; }
    [[nodiscard]] bool isDefined() const { return !sourceSize.isEmpty(); }
    std::expected<void, QString> validate() const;
    std::expected<QPointF, QString> fromSource(QPointF point) const;
    std::expected<QPointF, QString> toSource(QPointF point) const;
    std::expected<QPolygonF, QString> mapOutline(const QPolygonF &points, bool inverse = false) const;
    void appendProjective(QString operation, QTransform mapping, QSizeF output);
    QJsonObject toJson() const;
    static std::expected<CoordinatePipeline, QString> fromJson(const QJsonObject &json);
    bool operator==(const CoordinatePipeline &) const = default;
};

// Anchors avoid accumulating axis-aligned bounding-box inflation through edits.
// A changed editor box is re-anchored before applying another transformation.
struct GeometryAnchor {
    QPolygonF outline;
    QVector<QPolygonF> fragments;
    QRect lastBounds;
    QVector<QRect> lastFragments;
};
QJsonObject geometryAnchorsJson(const QHash<QString, GeometryAnchor> &anchors);
std::expected<QHash<QString, GeometryAnchor>, QString> readGeometryAnchors(const QJsonObject &json);
QPolygonF rectangleOutline(QRectF rectangle);
QRect enclosingPixels(const QPolygonF &outline);

} // namespace yagf::core
