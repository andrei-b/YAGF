#include "pagerestoration.h"
#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <QPainter>
#include <cmath>
#include <numbers>
#include <numeric>

namespace yagf::processing::restoration {
namespace {
bool valid(const QImage &im)
{
    return !im.isNull() && im.width() < 32767 && im.height() < 32767 &&
           qint64(im.width()) * im.height() <= 80'000'000;
}
QImage paperRgb(const QImage &image)
{
    if (!image.hasAlphaChannel()) return image.convertToFormat(QImage::Format_RGB888);
    QImage paper(image.size(), QImage::Format_RGB888);
    paper.fill(Qt::white);
    QPainter painter(&paper);
    painter.drawImage(0, 0, image);
    return paper;
}

cv::Mat pixels(const QImage &rgb)
{
    return {rgb.height(), rgb.width(), CV_8UC3, const_cast<uchar *>(rgb.constBits()), size_t(rgb.bytesPerLine())};
}
QImage resultImage(const cv::Mat &m, const QImage &source)
{
    QImage out = QImage(m.data, m.cols, m.rows, m.step, QImage::Format_RGB888).copy();
    out.setDotsPerMeterX(source.dotsPerMeterX());
    out.setDotsPerMeterY(source.dotsPerMeterY());
    return out;
}
cv::Mat gray(const cv::Mat &rgb)
{
    cv::Mat g;
    cv::cvtColor(rgb, g, cv::COLOR_RGB2GRAY);
    return g;
}
// RMS gradient relative to local contrast. Use the weaker gradient direction
// so that horizontal smearing remains visible even when horizontal rules are crisp.
struct Focus { double score, contrast; };
Focus focus(const cv::Mat &tile)
{
    cv::Scalar mean, deviation;
    cv::meanStdDev(tile, mean, deviation);
    cv::Mat smooth, dx, dy;
    cv::GaussianBlur(tile, smooth, {3, 3}, .65);
    cv::Sobel(smooth, dx, CV_32F, 1, 0, 3, .125);
    cv::Sobel(smooth, dy, CV_32F, 0, 1, 3, .125);
    const double energy = std::min(dx.dot(dx), dy.dot(dy)) / tile.total();
    return {std::sqrt(energy) / (deviation[0] + 3), deviation[0]};
}
cv::Mat registrationImage(const cv::Mat &image, double &scale)
{
    scale = std::min(1., 1800. / std::max(image.cols, image.rows));
    cv::Mat small;
    cv::resize(image, small, {}, scale, scale, cv::INTER_AREA);
    auto g = gray(small);
    cv::createCLAHE(2.)->apply(g, g);
    return g;
}
double lineY(const QPolygonF &line, double x, double target)
{
    const auto it = std::lower_bound(line.begin(), line.end(), x, [](QPointF p, double v) { return p.x() < v; });
    if (it == line.begin() || it == line.end()) {
        const QPointF end = it == line.begin() ? line.front() : line.back();
        const double fadeWidth = std::max(8., .2 * (line.back().x() - line.front().x()));
        return std::lerp(end.y(), target, std::min(1., std::abs(x - end.x()) / fadeWidth));
    }
    const auto a = *(it - 1), b = *it;
    return std::lerp(a.y(), b.y(), (x - a.x()) / (b.x() - a.x()));
}

// Build the attenuated inverse of a sampled line-spread function.
cv::Mat wienerKernel(cv::Size size, double length, double degrees, double strength, BlurProfile profile)
{
    cv::Mat psf = cv::Mat::zeros(size, CV_32F);
    const double angle = degrees * std::numbers::pi / 180;
    const double noise = std::pow(10., -1. - 1.6 * strength);
    if (profile == BlurProfile::SoftFocus) {
        // Elliptical Gaussian transfer function, with length measured at half maximum.
        cv::Mat filter(size, CV_32FC2);
        const double sigma = length / 2.355;
        const double cosAngle = std::cos(angle), sinAngle = std::sin(angle);
        for (int y = 0; y < size.height; ++y) {
            auto *row = filter.ptr<cv::Vec2f>(y);
            const double fy = double(y <= size.height / 2 ? y : y - size.height) / size.height;
            for (int x = 0; x < size.width; ++x) {
                const double fx = double(x <= size.width / 2 ? x : x - size.width) / size.width;
                const double along = sigma * (fx * cosAngle + fy * sinAngle);
                const double across = .25 * sigma * (-fx * sinAngle + fy * cosAngle);
                const double response = std::exp(-2 * std::numbers::pi * std::numbers::pi * (along * along + across * across));
                row[x] = {float((1 + noise) * response / (response * response + noise)), 0};
            }
        }
        return filter;
    }
    const int samples = int(std::ceil(length * 8));
    for (int i = 0; i < samples; ++i) {
        const double d = length * ((i + .5) / samples - .5);
        const double x = d * std::cos(angle), y = d * std::sin(angle);
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        for (int j = 0; j < 2; ++j) for (int k = 0; k < 2; ++k)
                psf.at<float>((iy + j + size.height) % size.height, (ix + k + size.width) % size.width) +=
                    (1 - std::abs(x - ix - k)) * (1 - std::abs(y - iy - j)) / samples;
    }
    cv::Mat spectrum;
    cv::dft(psf, spectrum, cv::DFT_COMPLEX_OUTPUT);
    for (auto it = spectrum.begin<cv::Vec2f>(); it != spectrum.end<cv::Vec2f>(); ++it) {
        auto &v = *it;
        const float gain = (1 + noise) / (v.dot(v) + noise);
        v = {v[0] * gain, -v[1] * gain};
    }
    return spectrum;
}
}
Result sharpnessMap(const QImage &page) try
{
    if (!valid(page)) return std::unexpected(QStringLiteral("Unsupported scan dimensions."));
    QImage rgb = paperRgb(page);
    auto g = gray(pixels(rgb));
    cv::Mat map = pixels(rgb).clone();
    cv::Mat background, texture;
    cv::boxFilter(g, background, CV_8U, {15, 15});
    cv::absdiff(g, background, texture);
    constexpr int step = 48;
    for (int y = 0; y < g.rows; y += step)
        for (int x = 0; x < g.cols; x += step) {
            cv::Rect r(x, y, std::min(step, g.cols - x), std::min(step, g.rows - y));
            const auto f = focus(g(r));
            if (f.contrast < 3 || cv::mean(texture(r))[0] < 1.2) continue;
            const double opacity = .52 * std::clamp((.30 - f.score) / .22, 0., 1.);
            if (opacity > 0) cv::addWeighted(map(r), 1 - opacity, cv::Mat(r.size(), CV_8UC3, cv::Scalar(245, 65, 35)), opacity, 0, map(r));
        }
    return resultImage(map, page);
}
catch (const cv::Exception &e) { return std::unexpected(QString::fromUtf8(e.what())); }

Result mergeCaptures(const QImage &reference, const QImage &second) try
{
    if (!valid(reference) || !valid(second)) return std::unexpected(QStringLiteral("Unsupported capture dimensions."));
    const QImage a = paperRgb(reference), b = paperRgb(second);
    const auto first = pixels(a), other = pixels(b);
    double sa, sb;
    const auto pa = registrationImage(first, sa), pb = registrationImage(other, sb);
    std::vector<cv::KeyPoint> ka, kb;
    cv::Mat da, db;
    auto features = cv::SIFT::create(5000);
    features->detectAndCompute(pa, cv::noArray(), ka, da);
    features->detectAndCompute(pb, cv::noArray(), kb, db);
    const auto insufficient = QStringLiteral("Could not align these captures reliably. Use the same page with more shared, readable detail.");
    if (da.rows < 12 || db.rows < 12) return std::unexpected(insufficient);
    std::vector<std::vector<cv::DMatch> > matches;
    cv::BFMatcher(cv::NORM_L2).knnMatch(db, da, matches, 2);
    std::vector<cv::Point2f> src, dst;
    for (const auto &m : matches)
        if (m.size() == 2 && m[0].distance < .72 * m[1].distance) {
            src.push_back(kb[m[0].queryIdx].pt);
            dst.push_back(ka[m[0].trainIdx].pt);
        }
    if (src.size() < 12) return std::unexpected(insufficient);
    cv::Mat inliers;
    cv::Mat h = cv::findHomography(src, dst, cv::RANSAC, 2.5, inliers);
    if (h.empty() || cv::countNonZero(inliers) < std::max(12, int(src.size() / 3))) return std::unexpected(insufficient);
    std::vector<cv::Point2f> supported, hull;
    for (size_t i = 0; i < dst.size(); ++i) if (inliers.at<uchar>(int(i))) supported.push_back(dst[i]);
    cv::convexHull(supported, hull);
    if (cv::contourArea(hull) < .035 * pa.total()) return std::unexpected(insufficient);
    h = cv::Mat(cv::Matx33d(1 / sa, 0, 0, 0, 1 / sa, 0, 0, 0, 1)) * h * cv::Mat(cv::Matx33d(sb, 0, 0, 0, sb, 0, 0, 0, 1));
    cv::Mat warped, validMask;
    cv::warpPerspective(other, warped, h, first.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar::all(255));
    cv::warpPerspective(cv::Mat(other.size(), CV_8U, cv::Scalar(255)), validMask, h, first.size(), cv::INTER_NEAREST);
    cv::erode(validMask, validMask, cv::getStructuringElement(cv::MORPH_RECT, {5, 5}));
    if (cv::countNonZero(validMask) < .15 * first.total()) return std::unexpected(insufficient);
    const auto ga = gray(first), gb = gray(warped);
    constexpr int step = 48;
    cv::Mat weights((ga.rows + step - 1) / step, (ga.cols + step - 1) / step, CV_32F, cv::Scalar(0));
    for (int y = 0; y < weights.rows; ++y)
        for (int x = 0; x < weights.cols; ++x) {
            cv::Rect r(x * step, y * step, std::min(step, ga.cols - x * step), std::min(step, ga.rows - y * step));
            if (cv::countNonZero(validMask(r)) != r.area()) continue;
            const auto f = focus(ga(r)), s = focus(gb(r));
            if (s.contrast < 5 || s.score <= f.score + .012) continue;
            // Curved paper can move locally even after the global alignment succeeds.
            // Only use sharper tiles whose ink pattern agrees with the first exposure.
            cv::Mat correlation;
            cv::matchTemplate(ga(r), gb(r), correlation, cv::TM_CCOEFF_NORMED);
            const double agreement = std::clamp((correlation.at<float>(0, 0) - .35) / .25, 0., 1.);
            weights.at<float>(y, x) = agreement * std::clamp((s.score - f.score - .012) / .06, 0., 1.);
        }
    cv::Mat alpha;
    cv::resize(weights, alpha, first.size(), 0, 0, cv::INTER_LINEAR);
    cv::Mat out = first.clone();
    for (int y = 0; y < out.rows; ++y) {
        auto *row = out.ptr<cv::Vec3b>(y);
        const auto *aligned = warped.ptr<cv::Vec3b>(y);
        for (int x = 0; x < out.cols; ++x)
            if (validMask.at<uchar>(y, x)) {
                const float w = alpha.at<float>(y, x);
                for (int c = 0; c < 3; ++c) row[x][c] = cv::saturate_cast<uchar>(std::lerp(float(row[x][c]), float(aligned[x][c]), w));
            }
    }
    return resultImage(out, reference);
}
catch (const cv::Exception &e) { return std::unexpected(QString::fromUtf8(e.what())); }

std::expected<core::CoordinatePipeline,QString> straighteningGeometry(QSize page,QVector<QPolygonF> guides)
{
    if (page.isEmpty() || page.width()>=32767 || page.height()>=32767 || qint64(page.width())*page.height()>80'000'000 ||
        guides.size() < 2 || guides.size() > 12)
        return std::unexpected(QStringLiteral("Trace between two and twelve lines."));
    std::vector<double> targets;
    for (const auto &line : guides) {
        if (line.size() < 2 || line.last().x() - line.first().x() < 8) return std::unexpected(QStringLiteral("Trace each line from left to right."));
        double area = 0;
        for (qsizetype i = 0; i < line.size(); ++i) {
            const auto p = line[i];
            if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || p.x() < 0 || p.x() > page.width() - 1 ||
                p.y() <= 0 || p.y() >= page.height() - 1 || (i && p.x() <= line[i - 1].x()))
                return std::unexpected(QStringLiteral("Guides must lie inside the page and progress left to right."));
            if (i) area += (p.x() - line[i - 1].x()) * (p.y() + line[i - 1].y()) / 2;
        }
        targets.push_back(area / (line.last().x() - line.first().x()));
    }
    std::vector<int> order(guides.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::sort(order, {}, [&](int i) { return targets[i]; });
    core::CoordinatePipeline pipeline;pipeline.sourceSize=page;
    core::CoordinateStep field;field.operation="traced-dewarp";field.inputSize=field.outputSize=page;field.gridSize={129,129};
    const auto nodes=[&](double x) {
        QVector<QPointF> pairs{{0,0}};
        for(int index:order)pairs.append({lineY(guides[index],x-.5,targets[index])+.5,targets[index]+.5});
        pairs.append({double(page.height()),double(page.height())});return pairs;
    };
    // Validate every source column as well as the sampled mesh nodes.
    for(int x=0;x<page.width();++x) {
        const auto pairs=nodes(x+.5);
        for(int i=1;i<pairs.size();++i)if(pairs[i].x()-pairs[i-1].x()<1 || pairs[i].y()-pairs[i-1].y()<1)
            return std::unexpected(QStringLiteral("Guides cross or touch. Trace separate lines."));
    }
    for(int row=0;row<129;++row)for(int col=0;col<129;++col) {
        const double x=double(col)*page.width()/128,y=double(row)*page.height()/128;
        const auto pairs=nodes(x);int segment=0;
        while(segment+2<pairs.size() && y>pairs[segment+1].x())++segment;
        const auto a=pairs[segment],b=pairs[segment+1];
        const double destination=std::lerp(a.y(),b.y(),(y-a.x())/(b.x()-a.x()));
        field.displacement.append({0,destination-y});
    }
    pipeline.steps.append(std::move(field));if(auto valid=pipeline.validate();!valid)return std::unexpected(valid.error());return pipeline;
}

Result straighten(const QImage &page,QVector<QPolygonF> guides) try
{
    if(!valid(page))return std::unexpected(QStringLiteral("Invalid restoration image."));
    auto geometry=straighteningGeometry(page.size(),std::move(guides));if(!geometry)return std::unexpected(geometry.error());
    const auto &field=geometry->steps.first();
    const QImage rgb=paperRgb(page);const auto input=pixels(rgb);cv::Mat out(input.size(),input.type());
    // Vertical mesh inversion is piecewise linear. Reuse its column knots for
    // every output pixel instead of running a general inverse per pixel.
    for(int x=0;x<input.cols;++x) {
        const double gx=(x+.5)*128/input.cols;const int col=std::clamp(int(gx),0,127);const double u=gx-col;
        std::array<double,129> knots;
        for(int row=0;row<129;++row)knots[row]=double(row)*input.rows/128+std::lerp(field.displacement[row*129+col].y(),field.displacement[row*129+col+1].y(),u);
        int segment=0;
        for(int y=0;y<input.rows;++y) {
            const double destination=y+.5;
            while(segment<127 && destination>knots[segment+1])++segment;
            const double sample=(segment+(destination-knots[segment])/(knots[segment+1]-knots[segment]))*input.rows/128-.5;
            const int lo=std::clamp(int(std::floor(sample)),0,input.rows-1),hi=std::min(lo+1,input.rows-1);
            const double fraction=std::clamp(sample-lo,0.,1.);
            const auto a=input.at<cv::Vec3b>(lo,x),b=input.at<cv::Vec3b>(hi,x);auto &pixel=out.at<cv::Vec3b>(y,x);
            for(int channel=0;channel<3;++channel)pixel[channel]=cv::saturate_cast<uchar>(std::lerp(double(a[channel]),double(b[channel]),fraction));
        }
    }
    return resultImage(out,page);
}
catch(const cv::Exception &e){return std::unexpected(QString::fromUtf8(e.what()));}

Result deblur(const QImage &page, QRect region, double length, double degrees, double strength, QRect output, BlurProfile profile) try
{
    if (!valid(page) || !std::isfinite(length) || length < 1 || length > 64 || !std::isfinite(degrees) ||
        !std::isfinite(strength) || strength < 0 || strength > 1) return std::unexpected(QStringLiteral("Invalid deblur settings."));
    region = region.normalized().intersected(page.rect());
    if (region.isEmpty()) return std::unexpected(QStringLiteral("Select a region to deblur."));
    output = output.isNull() ? page.rect() : output.intersected(page.rect());
    if (output.isEmpty()) return std::unexpected(QStringLiteral("Empty preview."));
    QImage result = page.copy(output).convertToFormat(QImage::Format_RGBA8888);
    const auto work = output.intersected(region);
    if (work.isEmpty() || strength == 0) return result;
    constexpr int tile = 256, halo = 96, size = tile + 2 * halo;
    const auto kernel = wienerKernel({size, size}, length, degrees, strength, profile);
    for (int y = region.top() + (work.top() - region.top()) / tile * tile; y <= work.bottom(); y += tile)
        for (int x = region.left() + (work.left() - region.left()) / tile * tile; x <= work.right(); x += tile) {
            const QRect block = QRect(x, y, tile, tile).intersected(work);
            QRect requested(x - halo, y - halo, size, size), bounded = requested.intersected(page.rect());
            // Convert only this tile so moving the detail preview costs the same on large pages.
            const QImage sourceTile = paperRgb(page.copy(bounded));
            cv::Mat padded;
            cv::copyMakeBorder(pixels(sourceTile), padded,
                               bounded.top() - requested.top(), requested.bottom() - bounded.bottom(), bounded.left() - requested.left(),
                               requested.right() - bounded.right(), cv::BORDER_REPLICATE | cv::BORDER_ISOLATED);
            cv::Mat lum = gray(padded), floating, spectrum, recovered;
            lum.convertTo(floating, CV_32F);
            cv::dft(floating, spectrum, cv::DFT_COMPLEX_OUTPUT);
            cv::mulSpectrums(spectrum, kernel, spectrum, 0);
            cv::idft(spectrum, recovered, cv::DFT_SCALE | cv::DFT_REAL_OUTPUT);
            for (int py = block.top(); py <= block.bottom(); ++py) {
                auto *dst = result.scanLine(py - output.y());
                for (int px = block.left(); px <= block.right(); ++px) {
                    const int bx = px - x + halo, by = py - y + halo;
                    const int edge = std::min({px - region.left(), region.right() - px, py - region.top(), region.bottom() - py});
                    const float delta = (recovered.at<float>(by, bx) - lum.at<uchar>(by, bx)) * strength * std::min(1., edge / 6.);
                    for (int c = 0; c < 3; ++c) {
                        auto &v = dst[4 * (px - output.x()) + c];
                        v = cv::saturate_cast<uchar>(v + delta);
                    }
                }
            }
        }
    return result;
}
catch (const cv::Exception &e) { return std::unexpected(QString::fromUtf8(e.what())); }
}
