#include "imageprocessor.h"

#include <QPainter>
#include <QTransform>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace yagf::processing {
namespace {

std::array<int, 256> histogram(const QImage &gray)
{
    std::array<int, 256> values{};
    for (int y = 0; y < gray.height(); ++y) {
        const uchar *row = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) ++values[row[x]];
    }
    return values;
}

int percentile(const std::array<int, 256> &values, int total, double fraction)
{
    const int target = std::max(1, static_cast<int>(total * fraction));
    int count = 0;
    for (int value = 0; value < 256; ++value) {
        count += values[value];
        if (count >= target) return value;
    }
    return 255;
}

int otsuThreshold(const std::array<int, 256> &values, int total)
{
    double sum = 0;
    for (int value = 0; value < 256; ++value) sum += value * values[value];
    double backgroundSum = 0, bestVariance = -1;
    int backgroundCount = 0, best = 127;
    for (int value = 0; value < 256; ++value) {
        backgroundCount += values[value];
        if (backgroundCount == 0 || backgroundCount == total) continue;
        backgroundSum += value * values[value];
        const double foregroundCount = total - backgroundCount;
        const double delta = backgroundSum / backgroundCount - (sum - backgroundSum) / foregroundCount;
        const double variance = backgroundCount * foregroundCount * delta * delta;
        if (variance > bestVariance) { bestVariance = variance; best = value; }
    }
    return std::min(best, 220);
}

double projectionScore(std::span<const QPoint> ink, int height, int width, double angleDegrees)
{
    constexpr double pi = 3.14159265358979323846;
    const double slope = std::tan(angleDegrees * pi / 180.0);
    const int offset = static_cast<int>(std::ceil(width * std::tan(8.0 * pi / 180.0))) + 2;
    std::vector<int> rows(static_cast<size_t>(height + 2 * offset + 4));
    for (const QPoint &point : ink) {
        const int row = qRound(point.y() - slope * point.x()) + offset;
        if (row >= 0 && row < static_cast<int>(rows.size())) ++rows[static_cast<size_t>(row)];
    }
    double score = 0;
    for (int count : rows) score += static_cast<double>(count) * count;
    return score;
}

QImage medianDenoise(const QImage &source)
{
    const QImage gray = source.convertToFormat(QImage::Format_Grayscale8);
    QImage result(gray.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < gray.height(); ++y) {
        uchar *destination = result.scanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            if (x == 0 || y == 0 || x + 1 == gray.width() || y + 1 == gray.height()) {
                destination[x] = gray.constScanLine(y)[x];
                continue;
            }
            std::array<uchar, 9> neighborhood{};
            int index = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                const uchar *row = gray.constScanLine(y + dy);
                for (int dx = -1; dx <= 1; ++dx) neighborhood[index++] = row[x + dx];
            }
            std::ranges::nth_element(neighborhood, neighborhood.begin() + 4);
            destination[x] = neighborhood[4];
        }
    }
    return result;
}

QImage stretchContrast(const QImage &source)
{
    const QImage gray = source.convertToFormat(QImage::Format_Grayscale8);
    const auto values = histogram(gray);
    const int total = gray.width() * gray.height();
    const int low = percentile(values, total, 0.01);
    const int high = percentile(values, total, 0.99);
    if (high - low < 4 || (low <= 4 && high >= 251)) return source;
    QImage result(source.size(), QImage::Format_ARGB32);
    const auto stretch = [low, high](int value) { return std::clamp((value - low) * 255 / (high - low), 0, 255); };
    for (int y = 0; y < source.height(); ++y) {
        const auto *input = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        auto *output = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const QRgb pixel = input[x];
            output[x] = qRgba(stretch(qRed(pixel)), stretch(qGreen(pixel)),
                              stretch(qBlue(pixel)), qAlpha(pixel));
        }
    }
    return result;
}

} // namespace

std::expected<QualityAssessment, QString> ImageProcessor::assess(const QImage &source)
{
    if (source.isNull()) return std::unexpected(QStringLiteral("Image is empty."));
    const QImage sampled = (source.width() > 900 || source.height() > 900
        ? source.scaled(QSize(900, 900), Qt::KeepAspectRatio, Qt::SmoothTransformation)
        : source).convertToFormat(QImage::Format_Grayscale8);
    const auto values = histogram(sampled);
    const int total = sampled.width() * sampled.height();
    QualityAssessment quality;
    quality.lowLuminance = percentile(values, total, 0.01);
    quality.highLuminance = percentile(values, total, 0.99);
    const int threshold = otsuThreshold(values, total);
    const int x0 = sampled.width() / 20, x1 = sampled.width() - x0;
    const int y0 = sampled.height() / 20, y1 = sampled.height() - y0;
    std::vector<QPoint> ink;
    for (int y = y0; y < y1; y += 2) {
        const uchar *row = sampled.constScanLine(y);
        for (int x = x0; x < x1; x += 2)
            if (row[x] <= threshold && row[x] < 220) ink.emplace_back(x, y);
    }
    const int samples = ((x1 - x0 + 1) / 2) * ((y1 - y0 + 1) / 2);
    if (ink.size() < 80 || ink.size() > static_cast<size_t>(samples * 0.35)) return quality;
    const double baseline = projectionScore(ink, sampled.height(), sampled.width(), 0.0);
    double bestScore = baseline, bestAngle = 0;
    for (int step = -14; step <= 14; ++step) {
        const double angle = step * 0.5;
        if (const double score = projectionScore(ink, sampled.height(), sampled.width(), angle); score > bestScore) { bestScore = score; bestAngle = angle; }
    }
    const double coarse = bestAngle;
    for (int step = -5; step <= 5; ++step) {
        const double angle = coarse + step * 0.1;
        if (angle < -7.0 || angle > 7.0) continue;
        if (const double score = projectionScore(ink, sampled.height(), sampled.width(), angle); score > bestScore) { bestScore = score; bestAngle = angle; }
    }
    if (std::abs(bestAngle) >= 0.3 && bestScore > baseline * 1.04)
        quality.skewDegrees = bestAngle;
    return quality;
}

std::expected<QImage, QString> ImageProcessor::apply(const QImage &source,
                                                     const core::PreprocessSettings &settings)
{
    if (source.isNull()) return std::unexpected(QStringLiteral("Image is empty."));
    if (!std::isfinite(settings.deskewDegrees) || std::abs(settings.deskewDegrees) > 15.0)
        return std::unexpected(QStringLiteral("Deskew angle must be within 15 degrees."));
    if (settings.isIdentity()) return source;
    if (static_cast<qint64>(source.width()) * source.height() > 80'000'000)
        return std::unexpected(QStringLiteral("Image is too large to preprocess safely."));
    QImage current = source.convertToFormat(QImage::Format_ARGB32);
    if (settings.denoise) current = medianDenoise(current).convertToFormat(QImage::Format_ARGB32);
    if (settings.autoContrast) current = stretchContrast(current);
    if (std::abs(settings.deskewDegrees) >= 0.01) {
        QTransform rotation;
        rotation.rotate(-settings.deskewDegrees);
        const QImage rotated = current.transformed(rotation, Qt::SmoothTransformation);
        QImage white(rotated.size(), QImage::Format_ARGB32);
        white.fill(Qt::white);
        QPainter painter(&white);
        painter.drawImage(QPoint(0, 0), rotated);
        current = std::move(white);
    }
    return current;
}

} // namespace yagf::processing
