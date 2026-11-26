#include "pdfpagepainter.h"
#include "pdfpagelayout.h"
#include "pagebackground.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTextOption>
#include <algorithm>
#include <memory>

namespace yagf::processing::detail {

QSet<QString> formulaWordIds(const core::OcrPage &page)
{
    QSet<QString> ids;
    for (const auto &region : page.regions) {
        if (region.type != "equation" && region.type != "formula") continue;
        for (const auto &paragraph : region.paragraphs)
            for (const auto &line : paragraph.lines)
                for (const auto &id : line.wordIds) ids.insert(id);
    }
    return ids;
}

void drawWord(QPainter &painter, const core::OcrWord &word, int dpi, bool formula)
{
    if (word.text.isEmpty() || word.bounds.isEmpty()) return;
    QFont font;
    if (!word.fontFamily.isEmpty()) font.setFamily(word.fontFamily);
    // Optional ligature glyphs can lose their Unicode mapping in the PDF text layer.
    font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::PreferNoShaping));
    const int fontPixels = word.sourceFontSizePx > 0
        ? qRound(double(word.sourceFontSizePx) * std::max(1, word.fontSizePt) / std::max(1, word.sourceFontSizePt)) : 0;
    font.setPixelSize(std::clamp(fontPixels > 0 ? fontPixels : word.bounds.height(), 6, 512));
    font.setBold(word.bold);
    font.setItalic(word.italic);
    if (word.layoutBlock || word.text.contains('\n') || word.text.contains('\t')) {
        // Reflow passages with normal character spacing inside their source block.
        font.setPixelSize(fontPixels > 0 ? std::clamp(fontPixels, 8, 512) :
            std::clamp(word.fontSizePt > 0 ? qRound(word.fontSizePt * double(dpi) / 72.0) : 18, 8, 48));
        QString text = word.text;
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace('\r', '\n');
        text.replace('\n', QChar(0x2028));
        QTextLayout layout(text, font);
        QTextOption option;
        // A typeset fraction can be much narrower than its LaTeX source. Preserve
        // explicit lines and fit the hidden transcript without splitting commands.
        option.setWrapMode(formula ? QTextOption::NoWrap : QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        qreal height = 0, width = 0;
        layout.beginLayout();
        while (true) {
            auto line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(word.bounds.width());
            line.setPosition(QPointF(0, height));
            height += line.height();
            width = std::max(width, line.naturalTextWidth());
        }
        layout.endLayout();
        qreal scale = std::min<qreal>(1, word.bounds.height() / std::max<qreal>(1, height));
        if (formula && width > 0) scale = std::min(scale, word.bounds.width() / width);
        painter.save();
        painter.translate(word.bounds.topLeft());
        painter.scale(scale, scale);
        layout.draw(&painter, QPointF(0, 0));
        painter.restore();
        return;
    }
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    const qreal naturalWidth = std::max<qreal>(1.0, metrics.horizontalAdvance(word.text));
    const qreal scaleX = static_cast<qreal>(word.bounds.width()) / naturalWidth;
    painter.save();
    painter.translate(word.bounds.x(), word.bounds.y());
    painter.scale(scaleX, 1.0);
    painter.drawText(QPointF(0, metrics.ascent()), word.text);
    painter.restore();
}

namespace {
QFont formattedFont(const core::OcrWord &word, int dpi)
{
    QFont font;
    if (!word.fontFamily.isEmpty()) font.setFamily(word.fontFamily);
    font.setBold(word.bold);
    font.setItalic(word.italic);
    font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::PreferNoShaping));
    const int pixels = word.sourceFontSizePx > 0
        ? qRound(double(word.sourceFontSizePx) * std::max(1, word.fontSizePt) / std::max(1, word.sourceFontSizePt))
        : word.layoutBlock ? qRound(std::max(8, word.fontSizePt) * double(dpi) / 72.0) : qRound(word.bounds.height() * 1.3);
    font.setPixelSize(std::clamp(pixels, 6, 512));
    return font;
}

// Fit by changing font size and wrapping at the block width. Scaling a completed
// paragraph also shrinks its width and moves its lines away from the source layout.
void drawFormattedWord(QPainter &painter, const core::OcrWord &word, int dpi)
{
    if (word.text.isEmpty() || word.bounds.isEmpty()) return;
    QFont font = formattedFont(word, dpi);
    painter.save();
    const bool sizedLine = word.sourceFontSizePx > 0 && !word.layoutBlock &&
                           !word.text.contains('\n') && !word.text.contains('\t');
    const int margin = sizedLine ? QFontMetricsF(font).height() : 0;
    painter.setClipRect(word.bounds.adjusted(0, -margin, 0, margin), Qt::IntersectClip);
    const bool acrossLines = std::ranges::any_of(word.sourceBoxes, [&](const QRect &box) {
        return !word.sourceBoxes.isEmpty() && std::abs(box.center().y() - word.sourceBoxes.first().center().y()) >
            std::min(box.height(), word.sourceBoxes.first().height()) / 2;
    });
    if (!word.layoutBlock && !word.text.contains('\n') && acrossLines) {
        // A correction joined across lines still belongs to its separate image fragments.
        QVector<int> boundaries{0};
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, word.text);
        for (int end = finder.toNextBoundary(); end >= 0; end = finder.toNextBoundary()) boundaries.append(end);
        qint64 totalWidth = 0, usedWidth = 0;
        for (const auto &box : word.sourceBoxes) totalWidth += box.width();
        int start = 0;
        for (int i = 0; i < word.sourceBoxes.size(); ++i) {
            usedWidth += word.sourceBoxes[i].width();
            const int count = boundaries.size() - 1;
            const int end = i + 1 == word.sourceBoxes.size() ? word.text.size() :
                boundaries[std::clamp(int(count * usedWidth / std::max<qint64>(1, totalWidth)), 0, count)];
            auto fragment = word;
            fragment.bounds = word.sourceBoxes[i];
            fragment.sourceBoxes.clear();
            fragment.text = word.text.mid(start, end - start);
            drawFormattedWord(painter, fragment, dpi);
            start = end;
        }
    } else if (word.layoutBlock || word.text.contains('\n') || word.text.contains('\t')) {
        QString text = word.text;
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace('\r', '\n');
        text.replace('\n', QChar(0x2028));
        QRectF inkBounds;
        const auto makeLayout = [&](const QFont &candidate) {
            auto layout = std::make_unique<QTextLayout>(text, candidate);
            QTextOption option;
            option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            layout->setTextOption(option);
            const QFontMetricsF metrics(candidate);
            inkBounds = {};
            qreal y = 0;
            layout->beginLayout();
            while (true) {
                auto line = layout->createLine();
                if (!line.isValid()) break;
                line.setLineWidth(word.bounds.width());
                line.setPosition(QPointF(0, y));
                const QString content = text.mid(line.textStart(), line.textLength()).trimmed();
                if (!content.isEmpty()) inkBounds = inkBounds.united(metrics.tightBoundingRect(content)
                    .translated(0, y + line.ascent()));
                y += line.height();
            }
            layout->endLayout();
            return layout;
        };
        auto layout = makeLayout(font);
        const auto fits = [&] { return inkBounds.height() <= word.bounds.height() && inkBounds.width() <= word.bounds.width(); };
        if (!fits()) {
            int low = 6, high = font.pixelSize(), best = 6;
            while (low <= high) {
                const int middle = (low + high) / 2;
                font.setPixelSize(middle);
                layout = makeLayout(font);
                if (fits()) { best = middle; low = middle + 1; }
                else high = middle - 1;
            }
            font.setPixelSize(best);
            layout = makeLayout(font);
        }
        const qreal scale = std::min({1.0, word.bounds.width() / std::max(1.0, inkBounds.width()),
                                     word.bounds.height() / std::max(1.0, inkBounds.height())});
        painter.translate(word.bounds.topLeft());
        painter.scale(scale, scale);
        layout->draw(&painter, -inkBounds.topLeft());
    } else if (const auto scripts = word.inlineScripts(); !scripts.empty()) {
        QTextLayout layout(word.text, font);
        QList<QTextLayout::FormatRange> formats;
        for (const auto &glyph : scripts) {
            QTextCharFormat format;
            format.setVerticalAlignment(glyph.position == core::ScriptPosition::Superscript
                ? QTextCharFormat::AlignSuperScript : QTextCharFormat::AlignSubScript);
            formats.append({glyph.start, glyph.length, format});
        }
        layout.setFormats(formats);
        layout.beginLayout();
        auto line = layout.createLine();
        if (line.isValid()) line.setNumColumns(word.text.size());
        layout.endLayout();
        if (line.isValid()) {
            const QSizeF size(line.naturalTextWidth(), line.height());
            const qreal scale = std::min({1.0, word.bounds.width() / std::max(1.0, size.width()),
                                         word.bounds.height() / std::max(1.0, size.height())});
            painter.translate(word.bounds.topLeft());
            painter.scale(scale, scale);
            layout.draw(&painter, QPointF(0, 0));
        }
    } else {
        const QFontMetricsF metrics(font);
        const QRectF ink = metrics.tightBoundingRect(word.text);
        // Glyph-box height is not an em size. Once recognition supplied a
        // consistent block font, fit only horizontally to retain that size.
        const qreal scale = std::min({1.0, word.bounds.width() / std::max(1.0, ink.width()),
                                     sizedLine ? 1.0 : word.bounds.height() / std::max(1.0, ink.height())});
        painter.translate(word.bounds.topLeft());
        painter.scale(scale, sizedLine ? 1.0 : scale);
        painter.setFont(font);
        painter.drawText(-ink.topLeft(), word.text);
    }
    painter.restore();
}

}

bool drawFormattedPage(QPainter &painter, const PdfPageLayout &layout, const QImage &image,
                       const std::atomic_bool &cancelled, const QSet<QString> &formulaWords)
{
    if (cancelled.load()) return false;
    const auto &page = layout.page;
    const auto formulas = formulaWordIds(page) + formulaWords;
    const auto &graphics = layout.graphics;
    const auto &rasterWords = layout.rasterWords;
    const auto &textAreas = layout.textAreas;
    painter.save();
    painter.setClipRegion(QRegion(image.rect()).subtracted(graphics), Qt::IntersectClip);
    for (const auto &area : textAreas) painter.fillRect(area, paperColor(image, area));
    for (const auto &word : page.words) {
        if (rasterWords.contains(word.id)) continue;
        if (word.sourceBoxes.isEmpty()) painter.fillRect(word.bounds, paperColor(image, word.bounds));
        else for (const auto &box : word.sourceBoxes) painter.fillRect(box, paperColor(image, box));
    }
    painter.setPen(Qt::black);
    for (const auto &word : page.words) {
        if (cancelled.load()) { painter.restore(); return false; }
        if (rasterWords.contains(word.id) || formulaWords.contains(word.id)) {
            // Preserve content-stream reading order for retained raster text too.
            painter.save();
            painter.setClipping(false);
            painter.setOpacity(0);
            drawWord(painter, word, page.renderDpi, formulas.contains(word.id));
            painter.restore();
        } else drawFormattedWord(painter, word, page.renderDpi);
    }
    painter.restore();
    return true;
}

} // namespace yagf::processing::detail
