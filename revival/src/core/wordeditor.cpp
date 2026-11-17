#include "wordeditor.h"

#include <QUuid>

#include <algorithm>

namespace yagf::core {
namespace {

QString newId()
{
    return QStringLiteral("edited-word-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

void setText(OcrWord &word, const EditedWord &edit)
{
    word.text = edit.text;
    word.bold = edit.bold;
    word.italic = edit.italic;
    if (edit.fontSizePt > 0) word.fontSizePt = edit.fontSizePt;
    word.reviewed = true;
}

QVector<OcrWord> changedSpan(const OcrPage &page, const QVector<OcrWord> &old,
                             int oldStart, int oldEnd, const QVector<EditedWord> &replacement,
                             int newStart, int newEnd, int firstSelected)
{
    const int sourceCount = oldEnd - oldStart;
    const int resultCount = newEnd - newStart;
    QVector<OcrWord> result;
    if (resultCount == 0) return result;
    result.reserve(resultCount);
    if (sourceCount == resultCount) {
        for (int i = 0; i < resultCount; ++i) {
            OcrWord word = old.at(oldStart + i);
            setText(word, replacement.at(newStart + i));
            result.append(std::move(word));
        }
        return result;
    }
    if (sourceCount == 0) {
        const OcrWord *left = oldStart > 0 ? &old.at(oldStart - 1) :
            firstSelected > 0 ? &page.words.at(firstSelected - 1) : nullptr;
        const OcrWord *right = oldStart < old.size() ? &old.at(oldStart) :
            firstSelected + old.size() < page.words.size() ?
                &page.words.at(firstSelected + old.size()) : nullptr;
        const OcrWord *anchor = left ? left : right;
        const int y = anchor ? anchor->bounds.y() : 0;
        const int height = anchor ? anchor->bounds.height() : qMin(12, page.height);
        const int gapStart = left ? left->bounds.right() + 1 : right ? right->bounds.x() : 0;
        const int gapEnd = right && left && left->block == right->block &&
            left->paragraph == right->paragraph && left->line == right->line
            ? right->bounds.x() : page.width;
        const int available = std::max(1, gapEnd - gapStart);
        for (int i = 0; i < resultCount; ++i) {
            OcrWord word = anchor ? *anchor : OcrWord{};
            word.id = newId();
            word.originalText.clear();
            word.sourceWordIds.clear();
            word.confidence = -1;
            const int x = qBound(0, gapStart + available * i / resultCount, page.width - 1);
            const int end = qBound(x + 1, gapStart + available * (i + 1) / resultCount, page.width);
            word.bounds = QRect(x, y, end - x, height);
            setText(word, replacement.at(newStart + i));
            result.append(std::move(word));
        }
        return result;
    }

    QRect sourceBox;
    QStringList sourceIds, originalTexts;
    for (int i = oldStart; i < oldEnd; ++i) {
        const OcrWord &word = old.at(i);
        sourceBox = sourceBox.isNull() ? word.bounds : sourceBox.united(word.bounds);
        for (const QString &id : word.sourceWordIds)
            if (!sourceIds.contains(id)) sourceIds.append(id);
        if (!word.originalText.isEmpty()) originalTexts.append(word.originalText);
    }
    qint64 totalCharacters = 0;
    for (int i = newStart; i < newEnd; ++i)
        totalCharacters += std::max<qsizetype>(1, replacement.at(i).text.size());
    qint64 charactersBefore = 0;
    for (int i = 0; i < resultCount; ++i) {
        const EditedWord &edit = replacement.at(newStart + i);
        OcrWord word = old.at(oldStart);
        word.id = i == 0 ? word.id : newId();
        word.sourceWordIds = sourceIds;
        word.originalText = originalTexts.join(QLatin1Char(' '));
        word.confidence = -1;
        const int x = sourceBox.x() + sourceBox.width() * charactersBefore / totalCharacters;
        charactersBefore += std::max<qsizetype>(1, edit.text.size());
        const int end = sourceBox.x() + sourceBox.width() * charactersBefore / totalCharacters;
        word.bounds = QRect(qBound(0, x, page.width - 1), sourceBox.y(),
                           std::max(1, end - x), sourceBox.height());
        if (word.bounds.right() >= page.width) word.bounds.setRight(page.width - 1);
        setText(word, edit);
        result.append(std::move(word));
    }
    return result;
}
} // namespace

std::expected<OcrPage, QString> WordEditor::apply(const OcrPage &page,
    const QVector<int> &selection, const QVector<EditedWord> &replacement)
{
    if (selection.isEmpty() || page.width < 1 || page.height < 1)
        return std::unexpected(QStringLiteral("Select source words before editing."));
    for (int i = 0; i < selection.size(); ++i)
        if (selection.at(i) < 0 || selection.at(i) >= page.words.size() ||
            (i && selection.at(i) != selection.at(i - 1) + 1))
            return std::unexpected(QStringLiteral("Select consecutive words in reading order."));
    for (const EditedWord &word : replacement)
        if (word.text.isEmpty() || std::any_of(word.text.cbegin(), word.text.cend(),
                [](QChar character) { return character.isSpace(); }) ||
            word.fontSizePt < 0 || word.fontSizePt > 72)
            return std::unexpected(QStringLiteral("Invalid replacement word or font size."));

    QVector<OcrWord> old;
    for (int index : selection) old.append(page.words.at(index));
    const int n = old.size(), m = replacement.size();
    int prefix = 0;
    while (prefix < n && prefix < m && old.at(prefix).text == replacement.at(prefix).text) ++prefix;
    int suffix = 0;
    while (suffix < n - prefix && suffix < m - prefix &&
           old.at(n - suffix - 1).text == replacement.at(m - suffix - 1).text) ++suffix;
    const int middleOld = n - prefix - suffix;
    const int middleNew = m - prefix - suffix;
    if (static_cast<qint64>(middleOld) * middleNew > 2'000'000)
        return std::unexpected(QStringLiteral("Select a smaller text span for this edit."));
    QVector<int> lcs((middleOld + 1) * (middleNew + 1));
    const auto cell = [middleNew, &lcs](int i, int j) -> int & {
        return lcs[i * (middleNew + 1) + j];
    };
    for (int i = middleOld - 1; i >= 0; --i)
        for (int j = middleNew - 1; j >= 0; --j)
            cell(i, j) = old.at(prefix + i).text == replacement.at(prefix + j).text
                ? 1 + cell(i + 1, j + 1) : std::max(cell(i + 1, j), cell(i, j + 1));
    QVector<QPair<int, int>> anchors{{-1, -1}};
    for (int i = 0; i < prefix; ++i) anchors.append({i, i});
    for (int i = 0, j = 0; i < middleOld && j < middleNew;) {
        if (old.at(prefix + i).text == replacement.at(prefix + j).text) {
            anchors.append({prefix + i++, prefix + j++});
        } else if (cell(i + 1, j) >= cell(i, j + 1)) ++i;
        else ++j;
    }
    for (int i = 0; i < suffix; ++i)
        anchors.append({n - suffix + i, m - suffix + i});
    anchors.append({n, m});
    QVector<OcrWord> edited;
    for (int k = 1; k < anchors.size(); ++k) {
        const auto [previousOld, previousNew] = anchors.at(k - 1);
        const auto [nextOld, nextNew] = anchors.at(k);
        edited += changedSpan(page, old, previousOld + 1, nextOld,
                              replacement, previousNew + 1, nextNew, selection.first());
        if (nextOld < n) {
            OcrWord anchor = old.at(nextOld);
            setText(anchor, replacement.at(nextNew));
            edited.append(std::move(anchor));
        }
    }
    OcrPage result = page;
    QVector<OcrWord> words;
    words.reserve(page.words.size() - selection.size() + edited.size());
    for (int i = 0; i < selection.first(); ++i) words.append(page.words.at(i));
    words += edited;
    for (int i = selection.last() + 1; i < page.words.size(); ++i) words.append(page.words.at(i));
    result.words = std::move(words);
    result.rebuildHierarchy();
    return result;
}

} // namespace yagf::core
