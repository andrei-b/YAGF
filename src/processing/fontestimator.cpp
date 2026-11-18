#include "fontestimator.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace yagf::processing {
namespace {
struct Ink {
    QImage mask;
    QSize size;
};

Ink ink(const QImage &input, bool firstLine)
{
    if (input.isNull()) return {};
    QImage gray = input.convertToFormat(QImage::Format_Grayscale8);
    // Ignore light paper/antialiasing noise when locating the actual letter edges.
    int left = gray.width(), right = -1, top = gray.height(), bottom = -1;
    int emptyRows = 0;
    for (int y = 0; y < gray.height(); ++y) {
        const auto *row = gray.constScanLine(y);
        bool occupied = false;
        for (int x = 0; x < gray.width(); ++x) {
            if (row[x] >= 160) continue;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = y;
            occupied = true;
        }
        if (occupied) emptyRows = 0;
        else if (bottom >= 0 && ++emptyRows >= 3 && firstLine) break;
    }
    if (right < left || bottom < top) return {};
    const QRect bounds(QPoint(left, top), QPoint(right, bottom));
    return {gray.copy(bounds).scaled(96, 32, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), bounds.size()};
}

struct Raster {
    Ink ink;
    int pixelSize = 0;
};

Raster raster(const QString &text, QFont font, const QSize &sourceSize, bool passage)
{
    font.setPixelSize(64);
    const QRectF reference = QFontMetricsF(font).tightBoundingRect(text.left(120));
    if (reference.height() < 1) return {};
    const int pixels = std::clamp(qRound(64.0 * sourceSize.height() / reference.height()), 6, 256);
    font.setPixelSize(pixels);
    QString lineText = text;
    if (passage && QFontMetricsF(font).horizontalAdvance(text) > sourceSize.width() * 1.08) {
        QTextLayout layout(text, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        auto line = layout.createLine();
        if (!line.isValid()) { layout.endLayout(); return {}; }
        line.setLineWidth(sourceSize.width() + 2);
        lineText = text.left(line.textLength()).trimmed();
        layout.endLayout();
    }
    const QFontMetricsF metrics(font);
    const QRectF bounds = metrics.tightBoundingRect(lineText);
    const int width = qCeil(std::max(bounds.width(), metrics.horizontalAdvance(lineText))) + 12;
    const int height = qCeil(metrics.height()) + 12;
    if (width < 1 || width > 8192 || height > 1024) return {};
    QImage result(width, height, QImage::Format_RGB32);
    result.fill(Qt::white);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(Qt::black);
    painter.drawText(QPointF(6 - std::min<qreal>(0, bounds.left()), 6 + metrics.ascent()), lineText);
    painter.end();
    return {ink(result, false), pixels};
}

double distance(const Ink &source, const Ink &candidate)
{
    if (candidate.mask.isNull()) return std::numeric_limits<double>::infinity();
    double difference = 0;
    for (int y = 0; y < source.mask.height(); ++y) {
        const auto *a = source.mask.constScanLine(y);
        const auto *b = candidate.mask.constScanLine(y);
        for (int x = 0; x < source.mask.width(); ++x) difference += std::abs(int(a[x]) - int(b[x]));
    }
    const double ratio = (double(candidate.size.width()) / candidate.size.height()) /
                         (double(source.size.width()) / source.size.height());
    return difference / (255 * 96 * 32) + 0.15 * std::abs(std::log(ratio));
}

QStringList families()
{
    const QStringList installed = QFontDatabase::families();
    QStringList result;
    // Common metric-compatible faces cover serif, sans serif, and monospace.
    for (const auto &name : {"Liberation Serif", "Liberation Sans", "Liberation Mono",
                            "DejaVu Serif", "DejaVu Sans", "DejaVu Sans Mono",
                            "Noto Serif", "Noto Sans", "Times New Roman", "Arial",
                            "Courier New", "Ubuntu"})
        if (installed.contains(QLatin1String(name))) result.append(QLatin1String(name));
    const QString fallback = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    if (!result.contains(fallback)) result.append(fallback);
    return result;
}

struct Style {
    QString family;
    bool bold = false;
    bool italic = false;
    int size = 0;
    int sourcePixels = 0;
};

void apply(core::OcrWord &word, const Style &style)
{
    word.fontFamily = style.family;
    word.bold = style.bold;
    word.italic = style.italic;
    word.fontSizePt = word.sourceFontSizePt = style.size;
    word.sourceFontSizePx = style.sourcePixels;
}
}

void estimateTextFormatting(core::OcrPage &page, const QImage &image, std::stop_token stop,
                            const std::atomic_bool *cancelled)
{
    const auto interrupted = [&] { return stop.stop_requested() || (cancelled && cancelled->load()); };
    // Font rendering requires a GUI application, including for command-line tests.
    if (image.isNull() || !qobject_cast<QGuiApplication *>(QCoreApplication::instance())) return;
    const QStringList candidates = families();
    QVector<QPair<int, Style>> estimates;
    const int stride = std::max(1, int((page.words.size() + 255) / 256));
    const double dpi = image.dotsPerMeterY() > 0 ? image.dotsPerMeterY() * 0.0254 : 150.0;
    for (int index = 0; index < page.words.size(); index += stride) {
        if (interrupted()) return;
        const auto &word = page.words[index];
        if (word.reviewed || !image.rect().contains(word.bounds)) continue;
        QString text = word.originalText.trimmed();
        const bool passage = word.layoutBlock || text.contains('\n');
        text = text.section('\n', 0, 0).trimmed().left(passage ? 512 : 80);
        // Isolated punctuation, formulas, and non-Latin scripts need other font models.
        int letters = 0;
        bool latin = true;
        for (QChar c : text) {
            if (!c.isLetter()) continue;
            ++letters;
            if (c.script() != QChar::Script_Latin) latin = false;
        }
        if (letters < 3 || !latin || text.contains('\\') || text.contains('\t') || text.contains('$')) continue;
        QImage crop = image.copy(word.bounds);
        if (crop.width() > 2400 || crop.height() > 2400)
            crop = crop.scaled(2400, 2400, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const Ink source = ink(crop, passage);
        if (source.mask.isNull() || source.size.height() < 6) continue;
        double best = std::numeric_limits<double>::infinity();
        Style style;
        for (const auto &family : candidates) {
            for (bool bold : {false, true}) for (bool italic : {false, true}) {
                if (interrupted()) return;
                QFont font(family);
                font.setBold(bold);
                font.setItalic(italic);
                const Raster candidate = raster(text, font, source.size, passage);
                const double score = distance(source, candidate.ink);
                if (score < best) {
                    best = score;
                    const double scale = double(word.bounds.width()) / crop.width();
                    style = {family, bold, italic, std::clamp(qRound(candidate.pixelSize * scale * 72 / dpi), 6, 72),
                             std::clamp(qRound(candidate.pixelSize * scale), 1, 4096)};
                }
            }
        }
        // Leave unrelated graphics or badly mismatched OCR text unstyled.
        if (best < 0.32 && !style.family.isEmpty()) estimates.append({index, style});
    }
    for (int index = 0; index < page.words.size(); ++index) {
        if (interrupted()) return;
        auto &word = page.words[index];
        if (word.reviewed) continue;
        const QPair<int, Style> *nearest = nullptr;
        for (const auto &estimate : estimates) {
            const auto &other = page.words[estimate.first];
            // Short tokens inherit the style of a nearby word on the same OCR line.
            if (index != estimate.first && (word.layoutBlock || other.layoutBlock || word.block != other.block ||
                word.paragraph != other.paragraph || word.line != other.line)) continue;
            if (!nearest || std::abs(index - estimate.first) < std::abs(index - nearest->first)) nearest = &estimate;
        }
        if (nearest) apply(word, nearest->second);
    }
}

} // namespace yagf::processing
