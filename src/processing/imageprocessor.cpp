#include "imageprocessor.h"
#include <QBuffer>
#include <QImageReader>

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

// A separable local mean needs only image-sized byte buffers, not a large
// 64-bit integral image. Dark ink is measured relative to its nearby paper.
QImage localDarkness(const QImage &image)
{
    const QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
    const int width = gray.width(), height = gray.height();
    const int radius = std::clamp(std::min(width, height) / 60, 15, 75);
    QImage horizontal(gray.size(), QImage::Format_Grayscale8);
    QImage darkness(gray.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < height; ++y) {
        const uchar *row = gray.constScanLine(y);
        uchar *output = horizontal.scanLine(y);
        int sum = 0;
        for (int x = 0; x < std::min(width, radius); ++x) sum += row[x];
        for (int x = 0; x < width; ++x) {
            if (x + radius < width) sum += row[x + radius];
            if (x - radius - 1 >= 0) sum -= row[x - radius - 1];
            const int count = std::min(width - 1, x + radius) - std::max(0, x - radius) + 1;
            output[x] = sum / count;
        }
    }
    std::vector<int> sums(width);
    const auto accumulateRow = [&](int y, int sign) {
        const auto *row = horizontal.constScanLine(y);
        for (int x = 0; x < width; ++x) sums[x] += sign * row[x];
    };
    for (int y = 0; y < std::min(height, radius); ++y) accumulateRow(y, 1);
    for (int y = 0; y < height; ++y) {
        if (y + radius < height) accumulateRow(y + radius, 1);
        if (y > radius) accumulateRow(y - radius - 1, -1);
        const int count = std::min(height - 1, y + radius) - std::max(0, y - radius) + 1;
        auto *out = darkness.scanLine(y);
        const auto *row = gray.constScanLine(y);
        for (int x = 0; x < width; ++x) out[x] = std::max(0, sums[x] / count - row[x]);
    }
    return darkness;
}

QImage smoothGrain(const QImage &source)
{
    // Separable 1-2-1 Gaussian kernel. The small support preserves narrow strokes.
    const auto gray = source.convertToFormat(QImage::Format_Grayscale8);
    QImage horizontal(gray.size(), QImage::Format_Grayscale8);
    QImage result(gray.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < gray.height(); ++y) {
        const auto *row = gray.constScanLine(y);
        auto *out = horizontal.scanLine(y);
        for (int x = 0; x < gray.width(); ++x)
            out[x] = (row[std::max(0, x-1)] + 2*row[x] + row[std::min(gray.width()-1, x+1)] + 2) / 4;
    }
    for (int y = 0; y < gray.height(); ++y) {
        const auto *above = horizontal.constScanLine(std::max(0, y-1));
        const auto *row = horizontal.constScanLine(y);
        const auto *below = horizontal.constScanLine(std::min(gray.height()-1, y+1));
        auto *out = result.scanLine(y);
        for (int x = 0; x < gray.width(); ++x)
            out[x] = (above[x] + 2*row[x] + below[x] + 2) / 4;
    }
    return result;
}

QImage adaptiveBinarize(const QImage &image, int inkThreshold = 0)
{
    QImage darkness = localDarkness(inkThreshold ? smoothGrain(image) : image);
    if (inkThreshold == 0) {
        for (int y = 0; y < darkness.height(); ++y) {
            uchar *row = darkness.scanLine(y);
            for (int x = 0; x < darkness.width(); ++x) row[x] = row[x] > 10 ? 0 : 255;
        }
        return darkness;
    }
    QImage binary(darkness.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < darkness.height(); ++y) {
        const uchar *row = darkness.constScanLine(y);
        uchar *out = binary.scanLine(y);
        for (int x = 0; x < darkness.width(); ++x) {
            bool ink = row[x] > inkThreshold;
            // Retain weak antialiased edges immediately touching a strong stroke.
            if (!ink && row[x] > 3 * inkThreshold / 4) {
                for (int dy = std::max(0, y-1); dy <= std::min(y+1, darkness.height()-1) && !ink; ++dy) {
                    const auto *neighbor = darkness.constScanLine(dy);
                    for (int dx = std::max(0, x-1); dx <= std::min(x+1, darkness.width()-1); ++dx)
                        ink |= neighbor[dx] > inkThreshold;
                }
            }
            out[x] = ink ? 0 : 255;
        }
    }
    return binary;
}

double projectionScore(std::span<const QPoint> ink, int height, int width, double angleDegrees)
{
    constexpr double pi = 3.14159265358979323846;
    const double slope = std::tan(angleDegrees * pi / 180.0);
    const int offset = static_cast<int>(std::ceil(width * std::tan(16.0 * pi / 180.0))) + 2;
    std::vector<int> rows(static_cast<size_t>(height + 2 * offset + 4));
    for (const QPoint &point : ink) {
        const int row = qRound(point.y() - slope * point.x()) + offset;
        if (row >= 0 && row < static_cast<int>(rows.size())) ++rows[static_cast<size_t>(row)];
    }
    double score = 0;
    for (int count : rows) score += static_cast<double>(count) * count;
    return score;
}

// Estimate paper brightness from the lighter pixels of each tile. Interpolate
// the estimates before dividing, so crossing a tile edge cannot split a stroke.
QImage levelPaper(const QImage &source)
{
    const auto gray = source.convertToFormat(QImage::Format_Grayscale8);
    const int tile = std::clamp(std::min(gray.width(), gray.height()) / 24, 16, 128);
    const int cols = std::max(1, gray.width() / tile), rows = std::max(1, gray.height() / tile);
    QImage paper(cols, rows, QImage::Format_Grayscale8);
    for (int r = 0; r < rows; ++r) {
        const int top = qint64(r) * gray.height() / rows, bottom = qint64(r+1) * gray.height() / rows;
        for (int c = 0; c < cols; ++c) {
            const int left = qint64(c) * gray.width() / cols, right = qint64(c+1) * gray.width() / cols;
            std::array<int, 256> counts{};
            for (int y = top; y < bottom; ++y) {
                const auto *line = gray.constScanLine(y);
                for (int x = left; x < right; ++x) ++counts[line[x]];
            }
            paper.scanLine(r)[c] = percentile(counts, (bottom-top)*(right-left), .85);
        }
    }
    const auto background = paper.scaled(gray.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                 .convertToFormat(QImage::Format_Grayscale8);
    QImage result(gray.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < gray.height(); ++y) {
        const auto *line = gray.constScanLine(y), *light = background.constScanLine(y);
        auto *out = result.scanLine(y);
        for (int x = 0; x < gray.width(); ++x)
            out[x] = std::min(255, 248 * line[x] / std::max(48, int(light[x])));
    }
    return result;
}

// Copy vertical strips without resampling narrow strokes. Extra white margin
// keeps content at both edges when the columns shift up or down.
QImage flattenBend(const QImage &source, int sag)
{
    QImage flat(source.width(), source.height() + std::abs(sag), QImage::Format_ARGB32);
    if (flat.isNull()) return {};
    flat.fill(Qt::white);
    QPainter painter(&flat);
    const auto shift = [&](int x) {
        // Rasterize the same bilinear displacement field recorded by the
        // coordinate pipeline, rounding only at the final pixel placement.
        const double grid=(x+.5)*128/source.width();const int col=std::clamp(int(grid),0,127);
        const auto offset=[&](int node){const double t=node/128.;return std::max(0,sag)-4.*sag*t*(1-t);};
        return qRound(std::lerp(offset(col),offset(col+1),grid-col));
    };
    for (int start = 0; start < source.width();) {
        const int offset = shift(start);
        int end = start+1;
        while (end < source.width() && shift(end) == offset) ++end;
        painter.drawImage(QPoint(start, offset), source, QRect(start, 0, end-start, source.height()));
        start = end;
    }
    return flat;
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

QImage ImageProcessor::normalizedTextPilot(const QImage &pilot)
{
    // Suppress isolated paper grain before leveling illumination. Keeping the same
    // canvas lets us use recovered word boxes to sample the original gray values.
    const QImage darkness = localDarkness(smoothGrain(pilot));
    const int threshold = std::max(4, otsuThreshold(histogram(darkness), darkness.width() * darkness.height()));
    QImage normalized(darkness.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < darkness.height(); ++y) {
        const auto *row = darkness.constScanLine(y);
        auto *out = normalized.scanLine(y);
        for (int x = 0; x < darkness.width(); ++x) out[x] = row[x] > threshold ? 0 : 255;
    }
    normalized.setDotsPerMeterX(pilot.dotsPerMeterX());
    normalized.setDotsPerMeterY(pilot.dotsPerMeterY());
    return normalized;
}

int ImageProcessor::estimateInkThreshold(const QImage &pilot, const QVector<QRect> &words)
{
    if (pilot.isNull() || words.size() < 3) return 0;
    const auto darkness = localDarkness(smoothGrain(pilot));
    QVector<int> thresholds;
    for (const QRect &box : words) {
        const auto rect = box.intersected(pilot.rect());
        if (rect.isEmpty()) continue;
        std::array<int, 256> counts{};
        // Limit the influence and cost of very large boxes.
        const int stride = std::max(1, std::max(rect.width(), rect.height()) / 100);
        int samples = 0;
        for (int y = rect.top(); y <= rect.bottom(); y += stride) {
            const auto *row = darkness.constScanLine(y);
            for (int x = rect.left(); x <= rect.right(); x += stride) {
                ++counts[row[x]];
                ++samples;
            }
        }
        const int split = otsuThreshold(counts, samples);
        int paperCount = 0, paperSum = 0, inkSum = 0;
        for (int i = 0; i < 256; ++i) {
            if (i <= split) { paperCount += counts[i]; paperSum += i * counts[i]; }
            else inkSum += i * counts[i];
        }
        const int inkCount = samples - paperCount;
        if (std::min(inkCount, paperCount) < std::max(3, samples / 20)) continue;
        const int paper = paperSum / paperCount, ink = inkSum / inkCount;
        if (ink - paper >= 8) thresholds.append((ink + paper) / 2);
    }
    if (thresholds.size() < 3) return 0;
    std::ranges::sort(thresholds);
    // Favor the lighter text samples on mixed-contrast pages.
    return std::clamp(thresholds[thresholds.size() / 4], 4, 64);
}

int ImageProcessor::recommendedRenderDpi(const QImage &pilot)
{
    if (pilot.isNull()) return 150;
    const auto gray = (pilot.width() > 2000 || pilot.height() > 2500
        ? pilot.scaled(QSize(2000, 2500), Qt::KeepAspectRatio, Qt::SmoothTransformation) : pilot)
        .convertToFormat(QImage::Format_Grayscale8);
    const int width = gray.width(), height = gray.height();
    const int threshold = otsuThreshold(histogram(gray), width * height);
    QVector<uchar> seen(width * height, 0);
    QVector<int> components, queue;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const int start = y * width + x;
        if (seen[start] || gray.constScanLine(y)[x] > threshold) continue;
        queue.clear(); queue.append(start); seen[start] = 1;
        int left = x, right = x, top = y, bottom = y;
        for (qsizetype i = 0; i < queue.size(); ++i) {
            const int px = queue[i] % width, py = queue[i] / width;
            left = std::min(left, px); right = std::max(right, px);
            top = std::min(top, py); bottom = std::max(bottom, py);
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const int nx = px + dx, ny = py + dy;
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                const int next = ny * width + nx;
                if (!seen[next] && gray.constScanLine(ny)[nx] <= threshold) { seen[next] = 1; queue.append(next); }
            }
        }
        const int componentHeight = qRound((bottom - top + 1) * double(pilot.height()) / height);
        const int componentWidth = qRound((right - left + 1) * double(pilot.width()) / width);
        if (componentHeight >= 3 && componentHeight <= 60 && componentWidth >= 2 && componentWidth <= 80 && queue.size() >= 3)
            components.append(componentHeight);
    }
    if (components.size() < 12) return 150;
    std::ranges::sort(components);
    const int glyph = components[components.size() / 2];
    return glyph >= 14 ? 150 : glyph >= 11 ? 200 : glyph >= 7 ? 300 : 400;
}

std::expected<QualityAssessment, QString> ImageProcessor::assess(const QImage &source)
{
    if (source.isNull()) return std::unexpected(QStringLiteral("Image is empty."));
    const QImage sampled = (source.width() > 1200 || source.height() > 1200
        ? source.scaled(QSize(1200, 1200), Qt::KeepAspectRatio, Qt::SmoothTransformation)
        : source).convertToFormat(QImage::Format_Grayscale8);
    const auto values = histogram(sampled);
    const int total = sampled.width() * sampled.height();
    QualityAssessment quality;
    quality.lowLuminance = percentile(values, total, 0.01);
    quality.highLuminance = percentile(values, total, 0.99);
    const QImage inkMask = adaptiveBinarize(sampled);
    const int x0 = sampled.width() / 20, x1 = sampled.width() - x0;
    const int y0 = sampled.height() / 20, y1 = sampled.height() - y0;
    std::vector<QPoint> ink;
    for (int y = y0; y < y1; ++y) {
        const uchar *row = inkMask.constScanLine(y);
        for (int x = x0; x < x1; ++x)
            if (row[x] == 0) ink.emplace_back(x, y);
    }
    const int samples = (x1 - x0) * (y1 - y0);
    if (ink.size() < 80 || ink.size() > static_cast<size_t>(samples * 0.35)) return quality;
    const double baseline = projectionScore(ink, sampled.height(), sampled.width(), 0.0);
    double bestScore = baseline, bestAngle = 0;
    for (int step = -30; step <= 30; ++step) {
        const double angle = step * 0.5;
        if (const double score = projectionScore(ink, sampled.height(), sampled.width(), angle); score > bestScore) { bestScore = score; bestAngle = angle; }
    }
    const double coarse = bestAngle;
    for (int step = -5; step <= 5; ++step) {
        const double angle = coarse + step * 0.1;
        if (angle < -15.0 || angle > 15.0) continue;
        if (const double score = projectionScore(ink, sampled.height(), sampled.width(), angle); score > bestScore) { bestScore = score; bestAngle = angle; }
    }
    if (std::abs(bestAngle) >= 0.3 && bestScore > baseline * 1.04)
        quality.skewDegrees = bestAngle;
    return quality;
}

std::expected<core::CoordinatePipeline,QString> ImageProcessor::coordinatePipeline(
    QSize sourceSize,const core::PreprocessSettings &settings,int pdfDpi)
{
    if(sourceSize.isEmpty() || sourceSize.width()>1000000 || sourceSize.height()>1000000 ||
        !std::isfinite(settings.deskewDegrees) || std::abs(settings.deskewDegrees)>15 ||
        settings.quarterTurns<0 || settings.quarterTurns>3 || !std::isfinite(settings.bendPercent) ||
        std::abs(settings.bendPercent)>core::PreprocessSettings::maxBendPercent || pdfDpi<0 || pdfDpi>10000)
        return std::unexpected(QStringLiteral("Invalid preprocessing geometry."));
    core::CoordinatePipeline pipeline;
    pipeline.sourceSize=sourceSize;
    if(!settings.restoredPng.isEmpty() && !settings.restorationGeometry.isDefined())pipeline.sourceFrame="restored";
    if(pdfDpi) {
        pipeline.sourceUnit="page-points-top-left";pipeline.sourceSize=QSizeF(sourceSize)*(72./pdfDpi);
        pipeline.appendProjective("render",QTransform::fromScale(pdfDpi/72.,pdfDpi/72.),sourceSize);
    }
    QSize current=sourceSize;
    if(!settings.restoredPng.isEmpty() && settings.restorationGeometry.isDefined()) {
        pipeline.sourceFrame=settings.restorationGeometry.sourceFrame;
        if(!settings.restorationGeometry.validate() || settings.restorationGeometry.sourceUnit!="pixels")
            return std::unexpected(QStringLiteral("Restoration geometry does not match the source raster."));
        if(settings.restorationGeometry.sourceSize!=QSizeF(current))pipeline.appendProjective("restoration-input-scale",
            QTransform::fromScale(settings.restorationGeometry.sourceSize.width()/current.width(),settings.restorationGeometry.sourceSize.height()/current.height()),settings.restorationGeometry.sourceSize);
        for(const auto &step:settings.restorationGeometry.steps)pipeline.steps.append(step);
        if(pipeline.outputSize()!=QSizeF(current))pipeline.appendProjective("restoration-output-scale",
            QTransform::fromScale(current.width()/pipeline.outputSize().width(),current.height()/pipeline.outputSize().height()),current);
    }
    if(settings.quarterTurns) {
        const QTransform rotation=QImage::trueMatrix(QTransform().rotate(settings.quarterTurns*90),current.width(),current.height());
        const QSize output=settings.quarterTurns%2 ? current.transposed() : current;
        pipeline.appendProjective("orientation",rotation,output);current=output;
    }
    if(!settings.perspective.isIdentity()) {
        const auto mapping=QTransform::fromScale(1./current.width(),1./current.height())*settings.perspective*QTransform::fromScale(current.width(),current.height());
        pipeline.appendProjective("perspective",mapping,current);
    }
    const int sag=qRound(settings.bendPercent*current.height()/100.);
    if(sag) {
        core::CoordinateStep step;step.operation="dewarp";step.inputSize=current;step.outputSize=QSize(current.width(),current.height()+std::abs(sag));step.gridSize={129,2};
        for(int row=0;row<2;++row)for(int col=0;col<129;++col) {
            const double t=col/128.;step.displacement.append({0,std::max(0,sag)-4.*sag*t*(1-t)});
        }
        pipeline.steps.append(step);current=step.outputSize.toSize();
    }
    if(std::abs(settings.deskewDegrees)>=.01) {
        const auto rotation=QTransform().rotate(-settings.deskewDegrees);
        const auto mapping=QImage::trueMatrix(rotation,current.width(),current.height());
        const auto outline=mapping.map(core::rectangleOutline(QRectF(QPointF(),current)));
        const auto output=core::enclosingPixels(outline).size();
        pipeline.appendProjective("deskew",mapping,output);
    }
    if(auto valid=pipeline.validate();!valid)return std::unexpected(valid.error());
    return pipeline;
}

std::expected<QImage, QString> ImageProcessor::apply(const QImage &source,
                                                     const core::PreprocessSettings &settings)
{
    if (source.isNull()) return std::unexpected(QStringLiteral("Image is empty."));
    if (!std::isfinite(settings.deskewDegrees) || std::abs(settings.deskewDegrees) > 15.0)
        return std::unexpected(QStringLiteral("Deskew angle must be within 15 degrees."));
    if (settings.quarterTurns < 0 || settings.quarterTurns > 3)
        return std::unexpected(QStringLiteral("Page orientation must be a multiple of 90 degrees."));
    if (!std::isfinite(settings.bendPercent) || std::abs(settings.bendPercent) > core::PreprocessSettings::maxBendPercent)
        return std::unexpected(QStringLiteral("Page bend is outside the supported range."));
    if (settings.isIdentity()) return source;
    if (static_cast<qint64>(source.width()) * source.height() > 80'000'000)
        return std::unexpected(QStringLiteral("Image is too large to preprocess safely."));
    QImage current = source;
    if (!settings.restoredPng.isEmpty()) {
        QBuffer buffer;
        buffer.setData(settings.restoredPng);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "PNG");
        const QSize size = reader.size();
        if (size.isEmpty() || qint64(size.width()) * size.height() > 80'000'000)
            return std::unexpected(QStringLiteral("Invalid restored page dimensions."));
        reader.setScaledSize(source.size());
        current = reader.read();
        if (current.isNull()) return std::unexpected(reader.errorString());
    }
    current = current.convertToFormat(QImage::Format_ARGB32);
    if (settings.quarterTurns)
        current = current.transformed(QTransform().rotate(settings.quarterTurns * 90), Qt::FastTransformation);
    if(!settings.perspective.isIdentity()) {
        core::CoordinatePipeline unit;unit.sourceSize={1,1};unit.appendProjective("perspective",settings.perspective,{1,1});
        if(auto valid=unit.validate();!valid)return std::unexpected(valid.error());
        QImage rectified(current.size(),QImage::Format_ARGB32);rectified.fill(Qt::white);
        const auto mapping=QTransform::fromScale(1./current.width(),1./current.height())*settings.perspective*QTransform::fromScale(current.width(),current.height());
        QPainter painter(&rectified);painter.setRenderHint(QPainter::SmoothPixmapTransform);painter.setTransform(mapping);painter.drawImage(QPoint(),current);painter.end();current=std::move(rectified);
    }
    if (settings.levelShadows) current = levelPaper(current).convertToFormat(QImage::Format_ARGB32);
    const int sag = qRound(settings.bendPercent * current.height() / 100.);
    if (sag) {
        if (qint64(current.width()) * (current.height() + std::abs(sag)) > 80'000'000)
            return std::unexpected(QStringLiteral("Straightened page is too large to preprocess."));
        current = flattenBend(current, sag);
        if (current.isNull()) return std::unexpected(QStringLiteral("Could not allocate the straightened page."));
    }
    if (settings.denoise) current = medianDenoise(current).convertToFormat(QImage::Format_ARGB32);
    if (settings.autoContrast) current = stretchContrast(current);
    if (settings.adaptiveBinarization) current = adaptiveBinarize(current, settings.inkThreshold).convertToFormat(QImage::Format_ARGB32);
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
    current.setDotsPerMeterX(source.dotsPerMeterX());
    current.setDotsPerMeterY(source.dotsPerMeterY());
    return current;
}

} // namespace yagf::processing
