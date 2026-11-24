#include "resultreview.h"
#include "formulatext.h"
#include "wordeditor.h"
#include <QJsonDocument>
#include <algorithm>

namespace yagf::intelligence {
QVector<ReviewBatch> reviewBatches(const core::OcrPage &page, QVector<int> words)
{
    QVector<ReviewBatch> batches;
    std::ranges::sort(words);
    words.erase(std::unique(words.begin(), words.end()), words.end());
    int size = 0;
    for (int index : words) {
        if (index < 0 || index >= page.words.size())
            continue;
        const auto source = core::formulaSourceText(page.words[index]);
        for (int start = 0; start < source.size();) {
            int end = std::min<qsizetype>(start + 2400, source.size());
            if (end < source.size()) {
                int split = end;
                while (split > start + 1200 && !source[split - 1].isSpace())
                    --split;
                if (split > start + 1200)
                    end = split;
                if (source[end - 1].isHighSurrogate())
                    --end;
            }
            if (batches.isEmpty() || batches.last().size() >= 48 || size + end - start > 6500) {
                batches.append(ReviewBatch {});
                size = 0;
            }
            batches.last().append(
                { index, start, QString("%1.%2").arg(index).arg(start), source.mid(start, end - start) });
            size += end - start;
            start = end;
        }
    }
    return batches;
}

std::expected<QVector<ReviewChange>, QString> readChanges(const ReviewBatch &batch, QString response)
{
    response = response.trimmed();
    if (response.startsWith("```") && response.endsWith("```")) {
        const auto start = response.indexOf('\n');
        if (start >= 0)
            response = response.mid(start + 1, response.size() - start - 4).trimmed();
    }
    const auto value = QJsonDocument::fromJson(response.toUtf8()).object().value("changes");
    if (!value.isArray() || value.toArray().size() > 256)
        return std::unexpected(
            QStringLiteral("The model returned an invalid review. Expected a JSON changes array."));
    const auto separators = [](const QString &text) {
        QString result;
        for (const auto c : text)
            if (c == '\t' || c == '\n' || c == '\r')
                result += c;
        return result;
    };
    QVector<ReviewChange> changes;
    for (const auto &entry : value.toArray()) {
        const auto o = entry.toObject();
        const auto part = std::ranges::find(batch, o["id"].toString(), &ReviewFragment::key);
        const auto before = o["before"].toString(), after = o["after"].toString();
        if (part == batch.end() || before.isEmpty() || !o["after"].isString() || before == after
            || after.size() > 4096 || separators(before) != separators(after))
            return std::unexpected(
                QStringLiteral("A suggested change did not identify a valid source passage."));
        const int at = part->text.indexOf(before);
        if (at < 0 || part->text.indexOf(before, at + 1) >= 0)
            return std::unexpected(
                QStringLiteral("A suggested change did not match one exact occurrence in its source passage."));
        ReviewChange change { part->word, part->offset + at, before, after, o["reason"].toString().left(500) };
        for (const auto &other : changes)
            if (change.word == other.word && change.offset < other.offset + other.before.size()
                && other.offset < change.offset + change.before.size())
                return std::unexpected(QStringLiteral("The model proposed overlapping changes."));
        changes.append(std::move(change));
    }
    return changes;
}

std::expected<core::OcrPage, QString> applyChanges(
    const core::OcrPage &page, QVector<ReviewChange> changes, const QString &model, bool unicodeMath)
{
    auto edited = page;
    // Later words and offsets first, so a verified split cannot move a pending edit.
    std::ranges::sort(changes,
        [](const auto &a, const auto &b) { return std::tie(a.word, a.offset) > std::tie(b.word, b.offset); });
    for (int n = 0; n < changes.size();) {
        const int index = changes[n].word;
        if (index < 0 || index >= page.words.size())
            return std::unexpected(QStringLiteral("The reviewed word is missing."));
        const auto &w = page.words[index];
        auto text = core::formulaSourceText(w);
        qsizetype end = text.size();
        do {
            const auto &c = changes[n++];
            if (c.offset < 0 || c.before.isEmpty() || c.offset + c.before.size() > end
                || text.mid(c.offset, c.before.size()) != c.before)
                return std::unexpected(QStringLiteral("The reviewed text has changed."));
            text.replace(c.offset, c.before.size(), c.after);
            end = c.offset;
        } while (n < changes.size() && changes[n].word == index);
        const bool formula = core::isFormulaWord(page, w);
        const bool structured = w.layoutBlock || !w.latexText.isEmpty() || !core::mathTextSpans(text).isEmpty();
        const auto displayed = unicodeMath ? core::unicodeFormulaText(text, formula).text : text;
        std::expected<core::OcrPage, QString> next;
        if (text.isEmpty())
            next = core::WordEditor::apply(edited, { index }, {});
        else if (structured)
            next = core::WordEditor::apply(edited, { index },
                { { displayed, w.bold, w.italic, w.fontSizePt, w.fontFamily,
                    w.layoutBlock || std::ranges::any_of(displayed, [](QChar c) { return c.isSpace(); }) } });
        else
            next = core::WordEditor::acceptPassage(edited, { index }, text);
        if (!next)
            return std::unexpected(next.error());
        const int count = next->words.size() - edited.words.size() + 1;
        for (int j = 0; j < count; ++j) {
            auto &accepted = next->words[index + j];
            accepted.readingEngine = "AI review · " + model.left(128);
            accepted.reviewed = true;
            accepted.alternative = {};
        }
        if (structured && count > 0)
            next->words[index].latexText = text != displayed ? text : QString {};
        edited = std::move(*next);
    }
    return edited;
}
}
