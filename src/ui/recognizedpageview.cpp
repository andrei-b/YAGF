#include "recognizedpageview.h"
#include "detail/pdfformulas.h"
#include "formulatext.h"

#include <QBuffer>
#include <QFutureWatcher>
#include <QMimeData>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QTextTable>
#include <QtConcurrentRun>
#include <algorithm>
#include <limits>

namespace yagf::ui {
namespace {
using core::OcrWord;
using FormulaKey = QPair<QString, bool>;
constexpr int formulaIndex = QTextFormat::UserProperty + 1;
constexpr int columnShare = formulaIndex + 1;
constexpr int naturalWidth = columnShare + 1;

struct Passage {
    QRect bounds;
    QVector<QVector<const OcrWord *>> lines;
    bool equation;
};

// Find clear gutters across a band of paragraphs. Full-width headings and
// footers split the page into bands before we look for gutters again.
using Group = QVector<const Passage *>;
QVector<Group> splitGroups(Group items, Qt::Orientation axis, int minimumGap)
{
    const auto start = [axis](const Passage *p) {
        return axis == Qt::Horizontal ? p->bounds.left() : p->bounds.top();
    };
    std::stable_sort(items.begin(), items.end(), [&](auto a, auto b) { return start(a) < start(b); });
    QVector<Group> groups;
    int end = 0;
    for (const auto *p : items) {
        if (groups.isEmpty() || start(p) - end > minimumGap) {
            groups.append(Group{});
            end = start(p);
        }
        groups.last().append(p);
        end = std::max(end, axis == Qt::Horizontal ? p->bounds.right() : p->bounds.bottom());
    }
    // The OCR hierarchy supplies reading order within each band or column.
    for (auto &group : groups) std::sort(group.begin(), group.end());
    return groups;
}

struct PageWriter {
    QVector<FormulaKey> formulas;
    QHash<FormulaKey, int> ids;
    int gutter;

    void math(QTextCursor &cursor, const QString &source, bool display, QTextCharFormat style, qreal share)
    {
        const FormulaKey key{source, display};
        if (!ids.contains(key)) {
            ids.insert(key, formulas.size());
            formulas.append(key);
        }
        style.setProperty(formulaIndex, ids.value(key));
        style.setProperty(columnShare, share);
        style.setToolTip(source);
        // Keep a multiline formula in one fragment so rendering replaces it once.
        auto fallback = core::unicodeMath(source).value_or(source);
        cursor.insertText(fallback.replace('\n', QChar::LineSeparator), style);
    }

    QTextCharFormat wordStyle(const OcrWord &word)
    {
        QTextCharFormat style;
        style.setFontItalic(word.italic);
        if (word.bold) style.setFontWeight(QFont::Bold);
        if (!word.fontFamily.isEmpty()) style.setFontFamilies({word.fontFamily});
        if (word.fontSizePt > 0) style.setFontPointSize(word.fontSizePt);
        return style;
    }

    void writePassage(QTextCursor &cursor, const Passage &p, qreal share)
    {
        QTextBlockFormat block;
        block.setBottomMargin(7);
        if (p.equation) block.setAlignment(Qt::AlignHCenter);
        static const QRegularExpression equationNumber(QStringLiteral(R"(^\(\d+(?:[.\-]\d+)*\)$)"));
        const bool number = p.lines.size() == 1 && p.lines.first().size() == 1 &&
                            equationNumber.match(p.lines.first().first()->text.trimmed()).hasMatch();
        if (number) block.setNonBreakableLines(true);
        cursor.setBlockFormat(block);
        bool afterDisplay = false;
        for (qsizetype i = 0; i < p.lines.size(); ++i) {
            if (p.equation) {
                if (i) cursor.insertBlock(block);
                QStringList parts;
                for (const auto *word : p.lines[i]) parts.append(core::formulaSourceText(*word));
                const QString line = parts.join(' ').trimmed();
                const auto spans = core::mathTextSpans(line);
                if (spans.isEmpty() || (spans.size() == 1 && spans.first().length == line.size())) {
                    math(cursor, spans.isEmpty() ? line : spans.first().source, true, wordStyle(*p.lines[i].first()), share);
                    continue;
                }
            }
            for (const auto *word : p.lines[i]) {
                if (afterDisplay) {
                    cursor.insertBlock(block);
                    afterDisplay = false;
                } else if (!cursor.atBlockStart()) cursor.insertText(" ", {});
                auto style = wordStyle(*word);
                if (number && word->fontSizePt == word->sourceFontSizePt) style.clearProperty(QTextFormat::FontPointSize);
                const auto text = core::formulaSourceText(*word);
                qsizetype pos = 0;
                for (const auto &span : core::mathTextSpans(text)) {
                    cursor.insertText(text.mid(pos, span.start - pos), style);
                    if (span.display && !cursor.atBlockStart()) cursor.insertBlock(block);
                    math(cursor, span.source, span.display, style, share);
                    pos = span.start + span.length;
                    if (span.display) {
                        if (pos == text.size()) afterDisplay = true;
                        else cursor.insertBlock(block);
                    }
                }
                cursor.insertText(text.mid(pos), style);
            }
        }
    }

    void write(QTextCursor &cursor, const Group &items, qreal share = 1.0)
    {
        if (items.isEmpty()) return;
        if (items.size() == 1) {
            writePassage(cursor, *items.first(), share);
            return;
        }
        const auto columns = splitGroups(items, Qt::Horizontal, gutter);
        QList<QTextLength> widths;
        int total = 0;
        int sharedTop = std::numeric_limits<int>::min();
        int sharedBottom = std::numeric_limits<int>::max();
        for (const auto &column : columns) {
            QRect bounds;
            for (const auto *p : column) bounds |= p->bounds;
            widths.append(QTextLength(QTextLength::PercentageLength, bounds.width()));
            total += bounds.width();
            sharedTop = std::max(sharedTop, bounds.top());
            sharedBottom = std::min(sharedBottom, bounds.bottom());
        }
        const auto bands = splitGroups(items, Qt::Vertical, 0);
        if (columns.size() > 1 && total > 0 && (sharedTop <= sharedBottom || bands.size() == 1)) {
            QTextTableFormat format;
            format.setWidth(QTextLength(QTextLength::PercentageLength, 100));
            format.setBorder(0);
            format.setCellPadding(0);
            format.setCellSpacing(6);
            // Keep narrow marginal text and equation numbers legible as the pane shrinks.
            const qreal minimum = 12.0 / widths.size();
            for (auto &width : widths)
                width = QTextLength(QTextLength::PercentageLength, minimum + 88.0 * width.rawValue() / total);
            format.setColumnWidthConstraints(widths);
            auto *table = cursor.insertTable(1, columns.size(), format);
            for (qsizetype c = 0; c < columns.size(); ++c) {
                auto cell = table->cellAt(0, c).firstCursorPosition();
                write(cell, columns[c], share * widths[c].rawValue() / 100);
            }
            cursor = table->lastCursorPosition();
            cursor.movePosition(QTextCursor::NextBlock);
            return;
        }
        if (bands.size() > 1) {
            for (const auto &band : bands) {
                if (!cursor.atBlockStart()) cursor.insertBlock();
                write(cursor, band, share);
            }
            return;
        }
        for (const auto *p : items) {
            if (!cursor.atBlockStart()) cursor.insertBlock();
            writePassage(cursor, *p, share);
        }
    }
};

// Walk backwards so replacing fallback text leaves later positions intact.
template<class Visitor>
void visitFormulas(QTextDocument *doc, Visitor visit)
{
    QVector<QTextFragment> fragments;
    for (auto b = doc->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().hasProperty(formulaIndex)) fragments.append(it.fragment());
    for (auto it = fragments.crbegin(); it != fragments.crend(); ++it) {
        QTextCursor cursor(doc);
        cursor.setPosition(it->position());
        cursor.setPosition(it->position() + it->length(), QTextCursor::KeepAnchor);
        visit(cursor, it->charFormat());
    }
}
}

RecognizedPageView::RecognizedPageView(QWidget *parent) : QTextEdit(parent)
{
    setObjectName("recognizedPageView");
    setReadOnly(true);
    setUndoRedoEnabled(false);
    setPlaceholderText(tr("Recognized text will appear here after OCR."));
    document()->setDocumentMargin(12);
}

RecognizedPageView::~RecognizedPageView()
{
    if (cancellation) cancellation->store(true);
}

void RecognizedPageView::setPage(const core::OcrPage *page)
{
    if (cancellation) cancellation->store(true);
    formulaImages.clear();
    clear();
    if (!page || page->words.isEmpty()) return;
    QHash<QString, const OcrWord *> words;
    for (const auto &word : page->words) words.insert(word.id, &word);
    QVector<Passage> passages;
    for (const auto &region : page->regions) {
        for (const auto &paragraph : region.paragraphs) {
            Passage passage{{}, {}, region.type == "formula" || region.type == "equation" || page->engineId == "rapid-latex-ocr"};
            for (const auto &line : paragraph.lines) {
                QVector<const OcrWord *> tokens;
                for (const auto &id : line.wordIds) {
                    const auto *word = words.value(id);
                    if (!word || word->text.isEmpty()) continue;
                    tokens.append(word);
                    passage.bounds |= word->bounds;
                }
                if (!tokens.isEmpty()) passage.lines.append(tokens);
            }
            if (!passage.lines.isEmpty()) passages.append(passage);
        }
    }
    Group all;
    for (const auto &p : passages) all.append(&p);
    PageWriter writer{{}, {}, std::max(1, page->width / 100)};
    QTextCursor cursor(document());
    writer.write(cursor, all);
    moveCursor(QTextCursor::Start);
    if (writer.formulas.isEmpty() || QStandardPaths::findExecutable("pdflatex").isEmpty() ||
        QStandardPaths::findExecutable("pdftoppm").isEmpty()) return;

    auto cancel = cancellation = std::make_shared<std::atomic_bool>(false);
    formulaImages.resize(writer.formulas.size());
    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, watcher, &QObject::deleteLater);
    connect(watcher, &QFutureWatcher<QImage>::resultReadyAt, this, [this, watcher, cancel](int id) {
        if (cancel->load()) return;
        const auto image = watcher->resultAt(id);
        if (image.isNull()) return;
        formulaImages[id] = image;
        int occurrence = 0;
        visitFormulas(document(), [&](QTextCursor &pos, const QTextCharFormat &style) {
            if (style.intProperty(formulaIndex) != id || style.isImageFormat()) return;
            QTextImageFormat format;
            format.setName(QStringLiteral("yagf-math:/%1/%2").arg(id).arg(occurrence++));
            format.setProperty(QTextFormat::ImageAltText, style.toolTip());
            format.setToolTip(style.toolTip());
            format.setVerticalAlignment(QTextCharFormat::AlignMiddle);
            format.setProperty(formulaIndex, id);
            format.setProperty(columnShare, style.doubleProperty(columnShare));
            const auto points = std::max(style.fontPointSize(), fontInfo().pointSizeF());
            format.setProperty(naturalWidth, image.width() * points / 10 * logicalDpiX() / 160);
            pos.insertImage(format);
        });
        sizeFormulaImages();
    });
    watcher->setFuture(QtConcurrent::run([formulas = std::move(writer.formulas), cancel](QPromise<QImage> &promise) {
        for (const auto &[source, display] : formulas) {
            if (cancel->load()) break;
            const auto rendered = processing::detail::renderFormulaPreview(source, display, *cancel);
            // Remove the editor preview's four-point padding (rasterized at 160 DPI).
            constexpr int margin = 8;
            promise.addResult(rendered ? rendered->copy(rendered->rect().adjusted(margin, margin, -margin, -margin)) : QImage{});
        }
    }));
}

void RecognizedPageView::sizeFormulaImages()
{
    if (formulaImages.isEmpty()) return;
    visitFormulas(document(), [&](QTextCursor &pos, const QTextCharFormat &style) {
        if (!style.isImageFormat()) return;
        auto image = style.toImageFormat();
        const qreal available = (viewport()->width() - 2 * document()->documentMargin()) *
                                style.doubleProperty(columnShare) - 12;
        const qreal width = std::max(1.0, std::min(style.doubleProperty(naturalWidth), available));
        if (qFuzzyCompare(width, image.width())) return;
        image.setWidth(width);
        const auto &original = formulaImages.at(style.intProperty(formulaIndex));
        image.setHeight(width * original.height() / original.width());
        auto scaled = original.scaledToWidth(std::max(1, qRound(width * devicePixelRatioF())), Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(devicePixelRatioF());
        document()->addResource(QTextDocument::ImageResource, image.name(), scaled);
        pos.setCharFormat(image);
    });
}

void RecognizedPageView::resizeEvent(QResizeEvent *event)
{
    QTextEdit::resizeEvent(event);
    sizeFormulaImages();
}

QMimeData *RecognizedPageView::createMimeDataFromSelection() const
{
    auto *mime = new QMimeData;
    const auto selection = textCursor().selection();
    QTextDocument copy;
    QTextCursor(&copy).insertFragment(selection);
    QHash<int, QString> urls;
    visitFormulas(&copy, [&](QTextCursor &cursor, const QTextCharFormat &style) {
        if (!style.isImageFormat()) return;
        const auto id = style.intProperty(formulaIndex);
        if (!urls.contains(id)) {
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            formulaImages.at(id).save(&buffer, "PNG");
            urls.insert(id, "data:image/png;base64," + QString::fromLatin1(png.toBase64()));
        }
        auto image = style.toImageFormat();
        image.setName(urls.value(id));
        cursor.setCharFormat(image);
    });
    mime->setHtml(copy.toHtml());
    visitFormulas(&copy, [](QTextCursor &cursor, const QTextCharFormat &style) {
        if (style.isImageFormat()) cursor.insertText(style.toolTip(), QTextCharFormat{});
    });
    mime->setText(copy.toPlainText());
    return mime;
}
}
