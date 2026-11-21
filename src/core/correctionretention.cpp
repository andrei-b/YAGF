#include "correctionretention.h"
#include "wordeditor.h"
#include "pagegeometry.h"

#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace yagf::core {
namespace {
QString normalized(const QString &text)
{
    return text.normalized(QString::NormalizationForm_KC).toCaseFolded()
        .remove(QRegularExpression(QStringLiteral(R"(\s+)")));
}

double similarity(const QString &a, const QString &b)
{
    if (a == b) return 1;
    if (a.isEmpty() || b.isEmpty()) return 0;
    // Large paragraph blocks often differ only in a small interior span.
    if (std::max(a.size(), b.size()) > 1024) {
        qsizetype prefix = 0, suffix = 0;
        while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) ++prefix;
        while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
               a[a.size() - suffix - 1] == b[b.size() - suffix - 1]) ++suffix;
        const auto left = a.mid(prefix, a.size() - prefix - suffix);
        const auto right = b.mid(prefix, b.size() - prefix - suffix);
        if (std::max(left.size(), right.size()) > 1024) return 0;
        const double distance = left.isEmpty() || right.isEmpty() ? std::max(left.size(), right.size())
            : (1 - similarity(left, right)) * std::max(left.size(), right.size());
        return 1 - distance / std::max(a.size(), b.size());
    }
    if (std::abs(a.size() - b.size()) * 3 > std::max(a.size(), b.size())) return 0;
    QVector<int> prior(b.size() + 1), row(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j) prior[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        row[0] = i;
        for (int j = 1; j <= b.size(); ++j)
            row[j] = std::min({row[j - 1] + 1, prior[j] + 1, prior[j - 1] + (a[i - 1] != b[j - 1])});
        prior.swap(row);
    }
    return 1.0 - double(prior.last()) / std::max(a.size(), b.size());
}

QRect mapBox(const QRect &box, const QRect &from, const QRect &to)
{
    const auto x = [&](int edge) { return to.x() + qRound(double(edge - from.x()) * to.width() / std::max(1, from.width())); };
    const auto y = [&](int edge) { return to.y() + qRound(double(edge - from.y()) * to.height() / std::max(1, from.height())); };
    const int left = x(box.left()), top = y(box.top());
    return QRect(left, top, std::max(1, x(box.right() + 1) - left), std::max(1, y(box.bottom() + 1) - top));
}

double overlap(const QRect &a, const QRect &b)
{
    const QRect common = a.intersected(b);
    const double intersection = double(common.width()) * common.height();
    const double area = double(a.width()) * a.height() + double(b.width()) * b.height() - intersection;
    return area > 0 ? intersection / area : 0;
}

bool sharesSource(const OcrWord &a, const OcrWord &b)
{
    for (const auto &id : a.sourceWordIds) if (b.sourceWordIds.contains(id)) return true;
    return false;
}
}

OcrPage rescalePageGeometry(const OcrPage &source, int width, int height, int sourceWidth, int sourceHeight, int dpi)
{
    if (width <= 0 || height <= 0 || sourceWidth <= 0 || sourceHeight <= 0 ||
        source.width <= 0 || source.height <= 0 || !validRenderDpi(dpi)) return source;
    if(source.coordinates.isDefined()) {
        auto destination=source.coordinates;
        destination.appendProjective("resolution",QTransform::fromScale(double(width)/source.width,double(height)/source.height),QSize(width,height));
        auto mapped=remapPageGeometry(source,std::move(destination));
        if(mapped) {
            mapped->sourceWidth=sourceWidth;mapped->sourceHeight=sourceHeight;mapped->renderDpi=dpi;
            for(auto &word:mapped->words)if(word.sourceFontSizePx>0)word.sourceFontSizePx=std::clamp(qRound(double(word.sourceFontSizePx)*height/source.height),1,4096);
            return *mapped;
        }
    }
    OcrPage page = source;
    const QRect from(0, 0, source.width, source.height), to(0, 0, width, height);
    const auto rescale = [&](OcrWord &word) {
        word.bounds = mapBox(word.bounds, from, to).intersected(to);
        for (auto &box : word.sourceBoxes) box = mapBox(box, from, to).intersected(word.bounds);
        if (word.sourceFontSizePx > 0) word.sourceFontSizePx = std::clamp(qRound(double(word.sourceFontSizePx) * height / std::max(1, source.height)), 1, 4096);
    };
    for (auto &word : page.words) rescale(word);
    for (auto &word : page.pendingCorrections) rescale(word);
    for (auto &word : page.deletedWords) rescale(word);
    for (auto &region : page.imageRegions) region.bounds = mapBox(region.bounds, from, to).intersected(to);
    for (auto &region : page.regions) region.bounds = mapBox(region.bounds, from, to).intersected(to);
    page.width = width; page.height = height;
    page.sourceWidth = sourceWidth; page.sourceHeight = sourceHeight;
    page.renderDpi = dpi;
    page.rebuildHierarchy();
    return page;
}

RetentionResult retainCorrections(OcrPage &fresh, const OcrPage &previous, bool preserveFormatting)
{
    RetentionResult report;
    OcrPage old;
    bool compatible=true;
    if(previous.coordinates.isDefined() && fresh.coordinates.isDefined()) {
        auto mapped=remapPageGeometry(previous,fresh.coordinates);
        if(mapped)old=std::move(*mapped);
        else {compatible=false;old=rescalePageGeometry(previous,fresh.width,fresh.height,fresh.sourceWidth,fresh.sourceHeight,fresh.renderDpi);}
    } else old=rescalePageGeometry(previous,fresh.width,fresh.height,fresh.sourceWidth,fresh.sourceHeight,fresh.renderDpi);
    fresh.imageRegions = old.imageRegions;
    fresh.deletedWords = old.deletedWords;
    fresh.pendingCorrections.clear();
    QVector<QVector<OcrWord>> groups;
    for (const auto &word : old.words) if (word.reviewed) {
        if (!groups.isEmpty() && sharesSource(groups.last().last(), word)) groups.last().append(word);
        else groups.append(QVector<OcrWord>{word});
    }
    QSet<QString> seen;
    for (const auto &group : groups) for (const auto &word : group) seen.insert(word.id);
    for (const auto &word : old.deletedWords) { groups.append(QVector<OcrWord>{word}); seen.insert(word.id); }
    for (const auto &word : old.pendingCorrections) if (!seen.contains(word.id)) {
        groups.append(QVector<OcrWord>{word}); seen.insert(word.id);
    }
    const auto incoming = fresh.words;
    QSet<int> used;
    QHash<int, QVector<OcrWord>> replacements;
    // Spatial bins keep long books from requiring a full page scan per correction.
    QHash<int, QVector<int>> rows;
    for (int i = 0; i < incoming.size(); ++i) {
        const auto &box = incoming[i].bounds;
        for (int row = box.top() * 64 / std::max(1, fresh.height); row <= box.bottom() * 64 / std::max(1, fresh.height); ++row)
            rows[row].append(i);
    }
    for (const auto &group : groups) {
        QRect bounds;
        QString original, corrected;
        QSet<QString> readings;
        for (const auto &word : group) {
            bounds |= word.bounds;
            if (!readings.contains(word.originalText)) { original += normalized(word.originalText); readings.insert(word.originalText); }
            corrected += normalized(word.text);
        }
        struct Match { int first = -1; int last = -1; double score = 0; QRect box; } best;
        double second = 0;
        QSet<int> candidates;
        for (int row = bounds.top() * 64 / std::max(1, fresh.height) - 1;
             row <= bounds.bottom() * 64 / std::max(1, fresh.height) + 1; ++row)
            for (int i : rows.value(row)) candidates.insert(i);
        for (int first : candidates) {
            if(!compatible)break;
            if (used.contains(first)) continue;
            QRect span;
            QString text;
            for (int last = first; last < incoming.size() && last < first + 16; ++last) {
                if (used.contains(last)) break;
                span |= incoming[last].bounds;
                text += normalized(incoming[last].text);
                const double geometry = overlap(bounds, span);
                if (geometry < 0.5) continue;
                const double content = std::max(similarity(original, text), similarity(corrected, text));
                if (content < 0.65) continue;
                const double score = geometry * 0.6 + content * 0.4;
                if (score > best.score) { second = best.score; best = {first, last, score, span}; }
                else second = std::max(second, score);
            }
        }
        if (best.first < 0 || best.score < 0.77 || best.score - second < 0.06) {
            for (auto word : group) { word.bounds = word.bounds.intersected(QRect(0, 0, fresh.width, fresh.height)); fresh.pendingCorrections.append(word); ++report.pending; }
            continue;
        }
        QVector<OcrWord> retained;
        for (auto word : group) {
            if (!word.text.isEmpty()) {
                word.bounds = mapBox(word.bounds, bounds, best.box).intersected(best.box);
                for (auto &box : word.sourceBoxes) box = mapBox(box, bounds, best.box).intersected(word.bounds);
                word.block = incoming[best.first].block; word.paragraph = incoming[best.first].paragraph; word.line = incoming[best.first].line;
                word.alternative = {};
                retained.append(word);
            }
            ++report.retained;
        }
        replacements.insert(best.first, retained);
        for (int i = best.first; i <= best.last; ++i) used.insert(i);
    }
    fresh.words.clear();
    QSet<QString> ids;
    for (int i = 0; i < incoming.size(); ++i) {
        if (replacements.contains(i)) fresh.words += replacements.value(i);
        else if (!used.contains(i)) {
            auto word = incoming[i];
            if (!preserveFormatting) for (const auto &prior : old.words)
                if (!prior.reviewed && prior.bounds == word.bounds && prior.originalText == word.originalText) {
                    word.fontFamily = prior.fontFamily; word.bold = prior.bold; word.italic = prior.italic;
                    word.fontSizePt = prior.fontSizePt; word.sourceFontSizePt = prior.sourceFontSizePt; word.sourceFontSizePx = prior.sourceFontSizePx;
                    break;
                }
            fresh.words.append(word);
        }
    }
    for (auto &word : fresh.words) {
        if (word.id.isEmpty() || ids.contains(word.id)) word.id = QStringLiteral("retained-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        ids.insert(word.id);
    }
    fresh.rebuildHierarchy();
    return report;
}

std::expected<OcrPage, QString> restorePendingCorrection(const OcrPage &page, const QString &id,
                                                        const QString &reading, const QVector<int> &selection)
{
    const auto it = std::find_if(page.pendingCorrections.begin(), page.pendingCorrections.end(),
                                 [&](const auto &word) { return word.id == id; });
    if (it == page.pendingCorrections.end()) return std::unexpected(QStringLiteral("The saved correction is no longer pending."));
    for (int i = 0; i < selection.size(); ++i)
        if (selection[i] < 0 || selection[i] >= page.words.size() || (i && selection[i] != selection[i - 1] + 1))
            return std::unexpected(QStringLiteral("Select consecutive current words for the saved correction."));
    auto saved = *it;
    OcrPage result = page;
    if (reading.isEmpty() && !selection.isEmpty())
        result.deletedWords.removeIf([&](const auto &word) { return word.id == id; });
    if (!selection.isEmpty()) {
        if (saved.layoutBlock && selection.size() == 1 && !reading.trimmed().isEmpty()) {
            auto &word = result.words[selection.first()];
            word.text = reading; word.reviewed = true; word.alternative = {};
            word.bold = saved.bold; word.italic = saved.italic; word.fontFamily = saved.fontFamily;
            word.fontSizePt = saved.fontSizePt; word.layoutBlock = true;
        } else {
            QVector<EditedWord> replacement;
            for (const auto &token : reading.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
                replacement.append({token, saved.bold, saved.italic, saved.fontSizePt, saved.fontFamily});
            auto edited = WordEditor::apply(result, selection, replacement);
            if (!edited) return std::unexpected(edited.error());
            result = std::move(*edited);
        }
    } else if (!reading.trimmed().isEmpty()) {
        saved.text = reading; saved.reviewed = true; saved.alternative = {};
        if (reading.contains(QRegularExpression(QStringLiteral("\\s")))) saved.layoutBlock = true;
        // No current words overlap: restore the saved source-linked item in spatial order.
        auto position = std::find_if(result.words.begin(), result.words.end(), [&](const auto &word) {
            return word.bounds.top() > saved.bounds.bottom() ||
                (word.bounds.center().y() >= saved.bounds.top() && word.bounds.center().y() <= saved.bounds.bottom() && word.bounds.left() > saved.bounds.right());
        });
        for (const auto &word : result.words) if (word.id == saved.id) { saved.id = QUuid::createUuid().toString(QUuid::WithoutBraces); break; }
        result.words.insert(position, saved);
    }
    result.pendingCorrections.removeIf([&](const auto &word) { return word.id == id; });
    result.rebuildHierarchy();
    return result;
}

} // namespace yagf::core
