#include "wordeditor.h"

#include <QSet>
#include <QUuid>

#include <algorithm>

namespace yagf::core {
namespace {

QString newId()
{
    return QStringLiteral("edited-word-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

void setText(OcrWord &word, const EditedWord &edit, bool reviewed = true)
{
    word.text = edit.text;
    word.bold = edit.bold;
    word.italic = edit.italic;
    if (edit.fontSizePt > 0) word.fontSizePt = edit.fontSizePt;
    word.reviewed = reviewed;
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
        const OcrWord *previous = oldStart > 0 ? &old.at(oldStart - 1) :
            firstSelected > 0 ? &page.words.at(firstSelected - 1) : nullptr;
        const OcrWord *next = oldStart < old.size() ? &old.at(oldStart) :
            firstSelected + old.size() < page.words.size() ?
                &page.words.at(firstSelected + old.size()) : nullptr;
        const OcrWord *anchor = previous ? previous : next;
        const int y = anchor ? anchor->bounds.y() : 0;
        const int height = anchor ? anchor->bounds.height() : qMin(12, page.height);
        const bool sameLine = previous && next && previous->block == next->block &&
            previous->paragraph == next->paragraph && previous->line == next->line;
        bool rtl = anchor && anchor->text.isRightToLeft();
        if (sameLine && previous->bounds.left() > next->bounds.right())
            rtl = true;
        else if (sameLine && previous->bounds.right() < next->bounds.left())
            rtl = false;
        // Measure offsets from the leading edge of the page in reading direction.
        int gapStart = 0, gapEnd = page.width;
        if (previous)
            gapStart = rtl ? page.width - previous->bounds.left() : previous->bounds.right() + 1;
        if (next && (!previous || sameLine))
            gapEnd = rtl ? page.width - next->bounds.right() - 1 : next->bounds.left();
        const int available = std::max(1, gapEnd - gapStart);
        qint64 totalCharacters = 0;
        for (int i = newStart; i < newEnd; ++i)
            totalCharacters += std::max<qsizetype>(1, replacement.at(i).text.size());
        qint64 charactersBefore = 0;
        for (int i = 0; i < resultCount; ++i) {
            OcrWord word = anchor ? *anchor : OcrWord{};
            word.id = newId();
            word.originalText.clear();
            word.sourceWordIds.clear();
            word.sourceBoxes.clear();
            word.confidence = -1;
            const int start = qBound(0, gapStart + available * charactersBefore / totalCharacters,
                                     page.width - 1);
            charactersBefore += std::max<qsizetype>(1, replacement.at(newStart + i).text.size());
            const int end = qBound(start + 1, gapStart + available * charactersBefore / totalCharacters,
                                   page.width);
            word.bounds = QRect(rtl ? page.width - end : start, y, end - start, height);
            setText(word, replacement.at(newStart + i));
            result.append(std::move(word));
        }
        return result;
    }

    struct Fragment {
        QRect box;
        int sourceIndex;
        bool actual;
        bool rtl;
        qint64 offset;
    };
    QVector<Fragment> fragments;
    qint64 totalWidth = 0;
    for (int i = oldStart; i < oldEnd; ++i) {
        const OcrWord &word = old.at(i);
        const bool actual = !word.sourceBoxes.isEmpty();
        const QVector<QRect> boxes = actual ? word.sourceBoxes : QVector<QRect>{word.bounds};
        const bool rtl = word.text.isRightToLeft();
        for (const QRect &box : boxes) {
            fragments.append({box, i, actual, rtl, totalWidth});
            totalWidth += box.width();
        }
    }
    qint64 totalCharacters = 0;
    for (int i = newStart; i < newEnd; ++i)
        totalCharacters += std::max<qsizetype>(1, replacement.at(i).text.size());
    qint64 charactersBefore = 0;
    QSet<QString> usedIds;
    for (int i = 0; i < resultCount; ++i) {
        const EditedWord &edit = replacement.at(newStart + i);
        const qint64 start = static_cast<qint64>(
            static_cast<long double>(totalWidth) * charactersBefore / totalCharacters);
        charactersBefore += std::max<qsizetype>(1, edit.text.size());
        const qint64 end = static_cast<qint64>(
            static_cast<long double>(totalWidth) * charactersBefore / totalCharacters);
        QVector<QRect> placement, sourceBoxes;
        QStringList sourceIds, originalTexts;
        QSet<QString> originalKeys;
        int firstContributor = -1;
        const auto addPiece = [&](const Fragment &fragment, qint64 begin, qint64 finish) {
            const int x = fragment.rtl
                ? fragment.box.right() + 1 - static_cast<int>(finish - fragment.offset)
                : fragment.box.left() + static_cast<int>(begin - fragment.offset);
            const QRect piece(x, fragment.box.y(), static_cast<int>(finish - begin),
                              fragment.box.height());
            placement.append(piece);
            if (fragment.actual) sourceBoxes.append(piece);
            const OcrWord &source = old.at(fragment.sourceIndex);
            if (firstContributor < 0) firstContributor = fragment.sourceIndex;
            for (const QString &id : source.sourceWordIds)
                if (!sourceIds.contains(id)) sourceIds.append(id);
            const QString key = source.sourceWordIds.join(QChar(0x1f));
            if (!source.originalText.isEmpty() && !originalKeys.contains(key)) {
                originalKeys.insert(key);
                originalTexts.append(source.originalText);
            }
        };
        for (const Fragment &fragment : fragments) {
            const qint64 begin = std::max(start, fragment.offset);
            const qint64 finish = std::min(end, fragment.offset + fragment.box.width());
            if (finish > begin) addPiece(fragment, begin, finish);
        }
        if (placement.isEmpty()) {
            const auto candidate = std::find_if(fragments.cbegin(), fragments.cend(),
                [start](const Fragment &fragment) {
                    return start < fragment.offset + fragment.box.width();
                });
            const Fragment &fragment = candidate == fragments.cend() ? fragments.last() : *candidate;
            const qint64 begin = qBound(fragment.offset, start,
                                       fragment.offset + fragment.box.width() - 1);
            addPiece(fragment, begin, begin + 1);
        }
        OcrWord word = old.at(firstContributor);
        word.id = usedIds.contains(word.id) ? newId() : word.id;
        usedIds.insert(word.id);
        word.sourceWordIds = sourceIds;
        word.originalText = originalTexts.join(QLatin1Char(' '));
        word.sourceBoxes = std::move(sourceBoxes);
        word.confidence = -1;
        word.bounds = placement.first();
        for (int j = 1; j < placement.size(); ++j)
            word.bounds = word.bounds.united(placement.at(j));
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
            // Editing one word in a passage does not verify its unchanged neighbors.
            const bool verifiedSelection = prefix == n && n == m;
            setText(anchor, replacement.at(nextNew), anchor.reviewed || verifiedSelection);
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
