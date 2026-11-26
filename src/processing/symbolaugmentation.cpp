#include "symbolaugmentation.h"
#include "symbolshape.h"
#include <QBuffer>
#include <QElapsedTimer>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

namespace yagf::processing {
namespace {
QImage paper(const QImage &image)
{
    QImage result(image.size(), QImage::Format_RGB32);
    result.fill(Qt::white);
    QPainter p(&result); p.drawImage(0, 0, image); p.end();
    return result.convertToFormat(QImage::Format_Grayscale8);
}
cv::Mat ink(const QImage &image)
{
    auto grey = paper(image);
    cv::Mat view(grey.height(), grey.width(), CV_8UC1, grey.bits(), grey.bytesPerLine()), binary;
    cv::threshold(view, binary, 0, 255, cv::THRESH_BINARY_INV|cv::THRESH_OTSU);
    return binary;
}
QRect rect(const cv::Rect &r) { return {r.x, r.y, r.width, r.height}; }
QString normalizedLanguage(QString code)
{
    if (code == "ru") return "rus";
    if (code == "en") return "eng";
    if (code == "de") return "deu";
    return code;
}
}
QByteArray symbolCropPng(const QImage &image, int maximum)
{
    QByteArray data;
    QBuffer buffer(&data); buffer.open(QIODevice::WriteOnly);
    (image.width() <= maximum && image.height() <= maximum ? image :
        image.scaled(maximum, maximum, Qt::KeepAspectRatio, Qt::SmoothTransformation)).save(&buffer, "PNG");
    return data;
}
QByteArray symbolFeature(const QImage &image)
{
    if (image.isNull() || image.width() > 20000 || image.height() > 20000) return {};
    const auto binary = ink(image);
    std::vector<cv::Point> points;
    cv::findNonZero(binary, points);
    if (points.size() < 4) return {};
    const auto bounds = cv::boundingRect(points);
    QImage normalized(binary.data, binary.cols, binary.rows, binary.step, QImage::Format_Grayscale8);
    auto inverted = normalized.copy(); inverted.invertPixels();
    return symbolShape(inverted, rect(bounds));
}
bool symbolLanguageMatches(const QString &rule, const QString &language)
{
    if (rule.isEmpty() || rule == "auto") return true;
    auto parts = language.split('+');
    for (auto code : rule.split('+')) for (auto other : parts)
        if (normalizedLanguage(code) == normalizedLanguage(other)) return true;
    return false;
}
QVector<SymbolCandidate> findSymbolCandidates(const QImage &image, const core::SymbolRule &rule,
                                             int dpi, std::stop_token stop, int maximum)
{
    QVector<SymbolCandidate> candidates;
    if (image.isNull() || stop.stop_requested()) return candidates;
    const auto seed = QImage::fromData(rule.seedPng, "PNG");
    const auto seedMask = symbolFeature(seed);
    if (seedMask.isEmpty()) return candidates;
    const double scale = double(dpi)/rule.seedDpi;
    const double height = rule.seedBounds.height()*scale, width = rule.seedBounds.width()*scale;
    const auto add = [&](QRect bounds, double similarity = -1.) {
        if (bounds.isEmpty() || !image.rect().contains(bounds)) return;
        const auto feature = symbolFeature(image.copy(bounds));
        if (feature.isEmpty()) return;
        const double score = similarity < 0 ? 1-core::symbolDistance(feature, seedMask) : similarity;
        if (score < .12) return;
        for (const auto &existing : candidates) {
            const auto overlap = bounds.intersected(existing.bounds);
            if (double(overlap.width())*overlap.height() > .6*std::min(bounds.width()*bounds.height(), existing.bounds.width()*existing.bounds.height())) return;
        }
        candidates.append({bounds, feature, score});
    };
    auto binary = ink(image);
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary.clone(), contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    for (const auto &contour : contours) {
        if (stop.stop_requested()) return {};
        const QRect r = rect(cv::boundingRect(contour));
        if (r.height() >= height*.5 && r.height() <= height*1.8 && r.width() >= width*.3 && r.width() <= width*2.2)
            add(r);
    }
    // Template matching also proposes symbols joined to neighboring ink, or
    // composed of disconnected strokes (for example a drawn smiley).
    const QImage seedGrey = paper(seed);
    for (const double factor : {.8, 1., 1.25}) {
        if (stop.stop_requested()) return {};
        const QSize size(std::max(3, qRound(width*factor)), std::max(3, qRound(height*factor)));
        if (size.width() >= image.width() || size.height() >= image.height()) continue;
        const auto scaled = seedGrey.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        auto templ = ink(scaled);
        if (cv::countNonZero(templ) < 4) continue;
        cv::Mat scores;
        cv::matchTemplate(binary, templ, scores, cv::TM_CCOEFF_NORMED);
        for (int count = 0; count < 48; ++count) {
            double best; cv::Point at;
            cv::minMaxLoc(scores, nullptr, &best, nullptr, &at);
            if (!std::isfinite(best) || best < .72) break;
            add({at.x, at.y, size.width(), size.height()}, best);
            const cv::Rect suppress(std::max(0, at.x-size.width()/2), std::max(0, at.y-size.height()/2), size.width(), size.height());
            scores(suppress & cv::Rect(0, 0, scores.cols, scores.rows)).setTo(-1);
        }
    }
    std::ranges::sort(candidates, [](const auto &a, const auto &b) { return a.score > b.score; });
    if (!maximum) return candidates;
    // Keep both strong matches and less similar candidates for negative review.
    QVector<SymbolCandidate> selected = candidates.mid(0, 32);
    for (int i = 32; i < candidates.size() && selected.size() < 48; i += std::max(1, int(candidates.size()-32)/16))
        selected.append(candidates[i]);
    return selected;
}

QVector<SymbolCut> symbolCuts(const QImage &image, const core::OcrWord &word,
                            const QString &language, std::stop_token stop)
{
    if (word.bounds.isEmpty() || !image.rect().contains(word.bounds) || word.originalText.isEmpty()) return {};
    if (word.originalText.toUcs4().size() == 1)
        return {{word.originalText, word.bounds, 0, int(word.originalText.size())}};
    QTemporaryDir temporary;
    if (!temporary.isValid()) return {};
    const QString path = temporary.filePath("word.png");
    // A white border prevents the single-line recognizer treating edge ink as
    // a page boundary. Box coordinates are translated back below.
    const auto crop = image.copy(word.bounds);
    QImage padded(crop.size()+QSize(12,12), QImage::Format_RGB32); padded.fill(Qt::white);
    QPainter painter(&padded); painter.drawImage(6,6,crop); painter.end();
    if (!padded.save(path)) return {};
    QStringList codes;
    for (auto code : language.split('+')) codes.append(normalizedLanguage(code));
    QProcess process;
    process.start("tesseract", {path, "stdout", "-l", codes.join('+'), "--psm", "7", "makebox"});
    if (!process.waitForStarted(2000)) return {};
    QElapsedTimer timer; timer.start();
    while (!process.waitForFinished(50)) {
        if (stop.stop_requested() || timer.elapsed() > 10000) { process.kill(); process.waitForFinished(); return {}; }
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode()) return {};
    QVector<SymbolCut> result;
    QString reading;
    for (auto row : QString::fromUtf8(process.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts)) {
        const auto parts = row.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() != 6) return {};
        bool ok[4]; int values[4];
        for (int i = 0; i < 4; ++i) values[i] = parts[i+1].toInt(&ok[i]);
        for (bool value : ok) if (!value) return {};
        const QRect box(values[0]-6, padded.height()-values[3]-6, values[2]-values[0], values[3]-values[1]);
        if (box.isEmpty() || !crop.rect().contains(box)) return {};
        result.append({parts[0], box.translated(word.bounds.topLeft()), int(reading.size()), int(parts[0].size())});
        reading += parts[0];
    }
    // Never guess character positions when a second reading changes the word.
    if (reading != word.originalText) return {};
    return result;
}

int applySymbolRules(core::OcrPage &page, const QImage &image, const QVector<core::SymbolRule> &rules,
                     std::stop_token stop)
{
    QVector<core::SymbolRule> active;
    for (const auto &r : rules) if (r.enabled && core::symbolRuleTrained(r) && symbolLanguageMatches(r.language, page.language)) active.append(r);
    if (active.isEmpty()) return 0;
    QVector<QRect> locations;
    struct Detection { QRect bounds; QByteArray mask; QString label; double score; };
    QVector<Detection> detections;
    for (const auto &r : active)
        for (const auto &candidate : findSymbolCandidates(image, r, page.renderDpi, stop, 0))
            if (const double score = core::symbolRuleScore(r, candidate.mask); score > 0) {
                locations.append(candidate.bounds); detections.append({candidate.bounds,candidate.mask,r.label,score});
            }
    if (locations.isEmpty()) return 0;
    int changed = 0;
    for (auto &word : page.words) {
        if (stop.stop_requested()) break;
        if (word.reviewed || word.layoutBlock || word.text != word.originalText || !image.rect().contains(word.bounds)) continue;
        if (!std::ranges::any_of(locations, [&](const QRect &r) { return word.bounds.contains(r.center()); })) continue;
        const auto cuts = symbolCuts(image, word, page.language, stop);
        QString output = word.text;
        for (auto cut = cuts.crbegin(); cut != cuts.crend(); ++cut) {
            const auto feature = symbolFeature(image.copy(cut->bounds));
            double best = 0, other = 0;
            QString label;
            for (const auto &r : active) {
                const double score = core::symbolRuleScore(r, feature);
                if (score > best) { if (label != r.label) other = best; best = score; label = r.label; }
                else if (r.label != label) other = std::max(other, score);
            }
            if (best > 0 && best-other >= .04 && !label.isEmpty()) output.replace(cut->start, cut->length, label);
        }
        if (output == word.text) continue;
        word.alternative = {page.engineId, word.text, {word.originalText}, false};
        word.text = output; word.readingEngine = QStringLiteral("symbol-augmentation"); ++changed;
    }
    // Recover a completely unread symbol only when an existing text line
    // anchors its reading order. Occupied boxes and non-text areas are retained.
    for (const auto &found : detections) {
        if (stop.stop_requested()) break;
        if (std::ranges::any_of(page.words,[&](const auto &w) { return w.bounds.intersects(found.bounds); })) continue;
        if (std::ranges::any_of(page.imageRegions,[&](const auto &r) { return
            (r.notText || r.content=="math") && r.bounds.intersects(found.bounds); })) continue;
        if (std::ranges::any_of(page.regions,[&](const auto &r) { return
            (r.isFigure() || r.type=="equation" || r.type=="formula" || r.type=="table") && r.bounds.intersects(found.bounds); })) continue;
        double other = 0;
        for (const auto &r : active) if (r.label != found.label) other = std::max(other,core::symbolRuleScore(r,found.mask));
        if (found.score-other < .04) continue;
        int neighbor = -1, bestGap = 100000;
        for (int i=0;i<page.words.size();++i) {
            const auto &w=page.words[i];
            const int h=std::max(w.bounds.height(),found.bounds.height());
            if (w.layoutBlock || std::abs(w.bounds.center().y()-found.bounds.center().y())>h*.6) continue;
            const int gap=std::max({0,found.bounds.left()-w.bounds.right(),w.bounds.left()-found.bounds.right()})+
                2*std::abs(w.bounds.center().y()-found.bounds.center().y());
            if (gap < bestGap && gap <= h*3) { neighbor=i; bestGap=gap; }
        }
        if (neighbor < 0) continue;
        const auto &anchor=page.words[neighbor];
        const bool ambiguous=std::ranges::any_of(page.words,[&](const auto &w) {
            if (w.layoutBlock || (w.block==anchor.block && w.paragraph==anchor.paragraph && w.line==anchor.line)) return false;
            const int h=std::max(w.bounds.height(),found.bounds.height());
            if (std::abs(w.bounds.center().y()-found.bounds.center().y())>h*.6) return false;
            const int gap=std::max({0,found.bounds.left()-w.bounds.right(),w.bounds.left()-found.bounds.right()})+
                2*std::abs(w.bounds.center().y()-found.bounds.center().y());
            return gap<=bestGap+h*.25;
        });
        if (ambiguous) continue;
        auto word=page.words[neighbor]; word.bounds=found.bounds; word.originalText=word.text=found.label;
        word.scripts.clear();
        word.id=QStringLiteral("page-%1/symbol-%2-%3-%4-%5").arg(page.number).arg(found.bounds.x()).arg(found.bounds.y()).arg(found.bounds.width()).arg(found.bounds.height());
        word.sourceWordIds={word.id}; word.sourceBoxes={found.bounds}; word.reviewed=false; word.confidence=-1;
        word.symbolShape=found.mask; word.alternative={}; word.readingEngine="symbol-augmentation";
        const bool after=found.bounds.center().x()>page.words[neighbor].bounds.center().x();
        page.words.insert(neighbor+(after?1:0),std::move(word)); ++changed;
    }
    if (changed) page.rebuildHierarchy();
    return changed;
}
} // namespace yagf::processing
