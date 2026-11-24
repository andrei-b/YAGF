#include "resultreviewdialog.h"
#include "formulatext.h"
#include <QBuffer>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStyle>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>

namespace yagf::ui {
namespace {
QLabel *label(const QString &text, const char *objectName = nullptr)
{
    auto *result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    if (objectName)
        result->setObjectName(QLatin1String(objectName));
    return result;
}
}

ResultReviewDialog::ResultReviewDialog(const core::OcrPage &source, QVector<int> words, QImage scan,
    bool unicodeOutput, const QString &sessionToken, QWidget *parent)
    : QDialog(parent)
    , page(source)
    , image(std::move(scan))
    , unicode(unicodeOutput)
    , batches(intelligence::reviewBatches(page, words))
{
    setObjectName("aiResultReview");
    const auto title = tr("AI-assisted review");
    setWindowTitle(title);
    resize(1040, 740);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(12);
    setStyleSheet(QStringLiteral(R"(
        #aiReviewTitle { font-size: 23px; font-weight: bold; }
        #aiReviewSection { font-size: 14px; font-weight: bold; }
        #aiReviewMeta, #aiReviewReason { color: palette(placeholder-text); }
        #aiReviewPaper { background: palette(base); border: 1px solid palette(midlight); border-radius: 8px; }
        #aiResultChanges { background: palette(window); border: none; }
        #aiReviewNote { background: palette(base); border: 1px solid palette(midlight); border-radius: 8px; }
        #aiReviewNote[active="true"] { border-color: palette(highlight); }
        #aiReviewBefore { text-decoration: line-through; color: palette(placeholder-text); }
        #aiReviewAfter { color: palette(link); font-weight: bold; }
        #aiReviewSourceLink { padding: 2px 4px; }
        #aiReviewScan { background: palette(base); border: 1px solid palette(midlight); border-radius: 8px; }
    )"));
    auto *header = new QHBoxLayout;
    header->addWidget(label(title, "aiReviewTitle"), 1);
    auto *settingsButton = new QToolButton(this);
    settingsButton->setText(tr("Review settings"));
    settingsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    settingsButton->setCheckable(true);
    header->addWidget(settingsButton);
    ask = new QPushButton(tr("Ask AI to review"), this);
    ask->setObjectName("askAiResult");
    header->addWidget(ask);
    layout->addLayout(header);

    auto *settingsPanel = new QWidget(this);
    auto *form = new QFormLayout;
    auto *settingsLayout = new QVBoxLayout(settingsPanel);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->addLayout(form);
    QSettings settings("YAGF", "YAGF Revival");
    endpoint = new QLineEdit(
        settings.value("ai/endpoint", "http://127.0.0.1:8080/v1/chat/completions").toString(), this);
    model = new QLineEdit(settings.value("ai/model").toString(), this);
    token = new QLineEdit(sessionToken, this);
    token->setEchoMode(QLineEdit::Password);
    endpoint->setObjectName("aiEndpoint");
    model->setObjectName("aiModel");
    form->addRow(tr("Chat-completions endpoint"), endpoint);
    form->addRow(tr("Model"), model);
    form->addRow(tr("Session token"), token);
    withImage = new QCheckBox(tr("Include the scan crop (vision models)"), this);
    withImage->setObjectName("aiIncludeScan");
    withImage->setEnabled(!image.isNull() && image.size() == QSize(page.width, page.height));
    directAnswer = new QCheckBox(tr("Request a direct answer"), this);
    directAnswer->setObjectName("aiDirectAnswer");
    directAnswer->setToolTip(tr("Ask a model that supports reasoning_effort=none to skip extended reasoning."));
    auto *options = new QHBoxLayout;
    options->addWidget(withImage);
    options->addWidget(directAnswer);
    options->addStretch();
    settingsLayout->addLayout(options);
    layout->addWidget(settingsPanel);
    connect(settingsButton, &QToolButton::toggled, this, [settingsButton, settingsPanel](bool open) {
        settingsButton->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        settingsPanel->setVisible(open);
    });
    settingsButton->setArrowType(Qt::RightArrow);
    settingsPanel->hide();
    settingsButton->setChecked(model->text().trimmed().isEmpty() || endpoint->text().trimmed().isEmpty());
    const auto loadDirectAnswer = [this] {
        QSettings saved("YAGF", "YAGF Revival");
        directAnswer->setChecked(saved.value("ai/directAnswerFor").toStringList()
            == QStringList{endpoint->text().trimmed(), model->text().trimmed()});
    };
    connect(endpoint, &QLineEdit::textChanged, this, loadDirectAnswer);
    connect(model, &QLineEdit::textChanged, this, loadDirectAnswer);
    loadDirectAnswer();
    if (words.size() == 1 && words.first() >= 0 && words.first() < page.words.size()) {
        const int i = words.first();
        QStringList context;
        const auto &target = page.words[i];
        for (int j = std::max(0, i - 4); j <= std::min(int(page.words.size()) - 1, i + 4); ++j) {
            const auto &w = page.words[j];
            if (w.block == target.block && w.paragraph == target.paragraph && w.line == target.line)
                context.append(j == i ? QStringLiteral("[…]") : w.text.left(100));
        }
        if (context.size() > 1)
            nearby = context.join(' ');
    }
    auto *split = new QSplitter(this);
    split->setChildrenCollapsible(false);
    auto *textPane = new QWidget(split);
    auto *textLayout = new QVBoxLayout(textPane);
    textLayout->setContentsMargins(0, 0, 8, 0);
    textLayout->addWidget(label(tr("Recognized text"), "aiReviewSection"));
    if (!nearby.isEmpty())
        textLayout->addWidget(label(tr("Nearby text · %1").arg(nearby), "aiReviewMeta"));
    transcription = new QTextBrowser(textPane);
    transcription->setObjectName("aiReviewPaper");
    transcription->setAccessibleName(tr("Recognized text"));
    transcription->setOpenLinks(false);
    connect(transcription, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        bool valid = false;
        const int index = url.path().toInt(&valid);
        if (url.scheme() == QStringLiteral("note") && valid && index >= 0 && index < notes.size())
            focusNote(index, false);
    });
    auto readingFont = transcription->font();
    readingFont.setFamily(QStringLiteral("Noto Serif"));
    readingFont.setStyleHint(QFont::Serif);
    readingFont.setPointSizeF(qMax(12.0, readingFont.pointSizeF() + 2));
    transcription->setFont(readingFont);
    transcription->document()->setDocumentMargin(22);
    renderTranscription();
    textLayout->addWidget(transcription, 1);

    auto *notesPane = new QWidget(split);
    notesPane->setMinimumWidth(250);
    auto *side = new QVBoxLayout(notesPane);
    side->setContentsMargins(8, 0, 0, 0);
    auto *notesHeader = new QHBoxLayout;
    notesHeader->addWidget(label(tr("Correction notes"), "aiReviewSection"), 1);
    noteCount = label({}, "aiReviewMeta");
    notesHeader->addWidget(noteCount);
    side->addLayout(notesHeader);
    margin = new QScrollArea(notesPane);
    margin->setObjectName("aiResultChanges");
    margin->setWidgetResizable(true);
    margin->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *noteContent = new QWidget;
    noteLayout = new QVBoxLayout(noteContent);
    noteLayout->setContentsMargins(1, 0, 4, 0);
    noteLayout->setSpacing(8);
    emptyNotes = label(tr("Suggestions appear here alongside the recognized text."), "aiReviewMeta");
    noteLayout->addWidget(emptyNotes);
    noteLayout->addStretch();
    margin->setWidget(noteContent);
    side->addWidget(margin, 1);
    scanPanel = new QWidget(notesPane);
    scanPanel->setObjectName("aiReviewScan");
    auto *scanLayout = new QVBoxLayout(scanPanel);
    scanLayout->addWidget(label(tr("Original scan"), "aiReviewSection"));
    preview = new QLabel;
    preview->setAlignment(Qt::AlignCenter);
    preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    scanLayout->addWidget(preview);
    side->addWidget(scanPanel);
    scanPanel->hide();
    split->addWidget(textPane);
    split->addWidget(notesPane);
    split->setSizes({620, 360});
    split->setStretchFactor(0, 1);
    layout->addWidget(split, 1);

    status = new QLabel(
        tr("Ask AI sends the recognized text to the selected model in %1 request(s).").arg(batches.size()), this);
    status->setObjectName("aiResultStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    apply = buttons->addButton(tr("Apply selected changes"), QDialogButtonBox::ActionRole);
    apply->setObjectName("applyAiResult");
    apply->setEnabled(false);
    auto *footer = new QHBoxLayout;
    footer->addWidget(status, 1);
    footer->addWidget(buttons);
    layout->addLayout(footer);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, [this] { chat.cancel(); });
    connect(ask, &QPushButton::clicked, this, [this] {
        if (batches.isEmpty())
            return;
        if (usedEndpoint != endpoint->text().trimmed() || usedModel != model->text().trimmed()
            || usedImage != withImage->isChecked() || usedDirectAnswer != directAnswer->isChecked()) {
            batchIndex = 0;
            activeNote = -1;
            const bool hadNotes = !changes.isEmpty();
            changes.clear();
            for (const auto &note : notes)
                delete note.card;
            notes.clear();
            emptyNotes->setText(tr("Suggestions appear here alongside the recognized text."));
            emptyNotes->show();
            scanPanel->hide();
            if (hadNotes)
                renderTranscription();
        }
        usedEndpoint = endpoint->text().trimmed();
        usedModel = model->text().trimmed();
        usedImage = withImage->isChecked();
        usedDirectAnswer = directAnswer->isChecked();
        sendBatch();
    });
    connect(apply, &QPushButton::clicked, this, [this] {
        QVector<intelligence::ReviewChange> selected;
        for (int i = 0; i < changes.size(); ++i)
            if (notes[i].check->isChecked())
                selected.append(changes[i]);
        if (selected.isEmpty())
            return;
        auto result = intelligence::applyChanges(page, selected, usedModel, unicode);
        if (!result) {
            status->setText(result.error());
            return;
        }
        edited = std::move(*result);
        accept();
    });
    ask->setEnabled(!batches.isEmpty());
}
QString ResultReviewDialog::sessionToken() const { return token->text(); }

void ResultReviewDialog::renderTranscription()
{
    QVector<int> order;
    order.reserve(changes.size());
    for (int i = 0; i < changes.size(); ++i)
        order.append(i);
    std::ranges::sort(order, {}, [this](int index) {
        return std::pair{changes[index].word, changes[index].offset};
    });
    auto *document = transcription->document();
    auto *scroll = transcription->verticalScrollBar();
    const int scrollPosition = scroll->value();
    document->clear();
    QTextCursor writer(document);
    writer.beginEditBlock();
    QTextBlockFormat paragraph;
    paragraph.setLineHeight(140, QTextBlockFormat::ProportionalHeight);
    paragraph.setBottomMargin(12);
    writer.setBlockFormat(paragraph);
    const QTextCharFormat plain;
    const auto write = [&writer](QString text, const QTextCharFormat &format) {
        text.replace(u'\r', QChar::LineSeparator);
        writer.insertText(text, format);
    };
    int last = -1;
    auto next = order.cbegin();
    for (const auto &batch : batches)
        for (const auto &part : batch) {
            if (last == part.word)
                continue;
            const auto &word = page.words[part.word];
            if (last >= 0) {
                const auto &previous = page.words[last];
                if (word.layoutBlock || previous.layoutBlock || word.block != previous.block
                    || word.paragraph != previous.paragraph || last + 1 != part.word)
                    writer.insertBlock(paragraph, plain);
                else
                    writer.insertText(word.line != previous.line ? QString(QChar::LineSeparator) : QStringLiteral(" "), plain);
            }
            const auto text = core::formulaSourceText(word);
            int from = 0;
            while (next != order.cend() && changes[*next].word == part.word) {
                const int index = *next++;
                const auto &change = changes[index];
                write(text.mid(from, change.offset - from), plain);
                notes[index].textStart = writer.position();
                QTextCharFormat original;
                original.setAnchor(true);
                original.setAnchorHref(QStringLiteral("note:%1").arg(index));
                original.setFontStrikeOut(true);
                write(change.before, original);
                if (!change.after.isEmpty()) {
                    auto replacement = original;
                    replacement.setForeground(transcription->palette().link());
                    replacement.setFontStrikeOut(false);
                    replacement.setFontWeight(QFont::DemiBold);
                    write(QChar(0x202f) + change.after, replacement);
                }
                notes[index].textEnd = writer.position();
                from = change.offset + change.before.size();
            }
            write(text.mid(from), plain);
            last = part.word;
        }
    writer.endEditBlock();
    writer.movePosition(QTextCursor::Start);
    transcription->setTextCursor(writer);
    scroll->setValue(scrollPosition);
    updateHighlights();
}

void ResultReviewDialog::appendNote(int index)
{
    const auto &change = changes[index];
    auto *card = new QWidget;
    card->setObjectName("aiReviewNote");
    card->setAttribute(Qt::WA_StyledBackground);
    auto *body = new QVBoxLayout(card);
    body->setContentsMargins(12, 10, 12, 9);
    body->setSpacing(5);
    const auto earlier = [&change](const auto &other) {
        return std::tie(other.word, other.offset) < std::tie(change.word, change.offset);
    };
    const auto replaceLabel = change.after.isEmpty() ? tr("Delete") : change.after;
    auto *original = label(change.before, "aiReviewBefore");
    original->setAccessibleName(tr("Original text: %1").arg(change.before));
    auto *replacement = label(replaceLabel, "aiReviewAfter");
    replacement->setAccessibleName(tr("Suggested text: %1").arg(replaceLabel));
    body->addWidget(original);
    body->addWidget(replacement);
    if (!change.reason.isEmpty())
        body->addWidget(label(change.reason, "aiReviewReason"));
    auto *actions = new QHBoxLayout;
    auto *check = new QCheckBox(tr("Select change"), card);
    check->setObjectName("aiNoteCheck");
    check->setAccessibleName(tr("Select change from %1 to %2").arg(change.before, replaceLabel));
    actions->addWidget(check);
    actions->addStretch();
    auto *locate = new QPushButton(tr("Show in text"), card);
    locate->setFlat(true);
    locate->setObjectName("aiReviewSourceLink");
    locate->setAutoDefault(false);
    actions->addWidget(locate);
    body->addLayout(actions);
    emptyNotes->hide();
    const auto position = std::count_if(changes.cbegin(), changes.cbegin() + index, earlier);
    noteLayout->insertWidget(1 + position, card);
    notes.append({check, card});
    connect(check, &QCheckBox::toggled, this, [this, index] {
        focusNote(index, true);
        updateApply();
    });
    connect(locate, &QPushButton::clicked, this, [this, index] { focusNote(index, true); });
}

void ResultReviewDialog::focusNote(int index, bool scrollToText)
{
    const auto setActive = [this](int at, bool active) {
        auto *card = notes[at].card;
        card->setProperty("active", active);
        card->style()->unpolish(card);
        card->style()->polish(card);
        card->update();
    };
    if (activeNote >= 0)
        setActive(activeNote, false);
    activeNote = index;
    setActive(index, true);
    updateHighlights();
    const auto &change = changes[index];
    if (scrollToText) {
        QTextCursor cursor(transcription->document());
        cursor.setPosition(notes[index].textStart);
        transcription->setTextCursor(cursor);
        transcription->ensureCursorVisible();
    }
    const auto box = page.words[change.word].bounds.intersected(image.rect());
    const bool showScan = image.size() == QSize(page.width, page.height) && !box.isEmpty();
    if (showScan)
        preview->setPixmap(QPixmap::fromImage(image.copy(box.adjusted(-3, -3, 3, 3).intersected(image.rect()))
            .scaled(QSize(218, 94), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    scanPanel->setVisible(showScan);
    margin->ensureWidgetVisible(notes[index].card, 0, 8);
}

void ResultReviewDialog::updateHighlights()
{
    QList<QTextEdit::ExtraSelection> selections;
    const auto accent = transcription->palette().color(QPalette::Highlight);
    for (int i = 0; i < changes.size(); ++i) {
        QTextEdit::ExtraSelection selection;
        selection.cursor = QTextCursor(transcription->document());
        selection.cursor.setPosition(notes[i].textStart);
        selection.cursor.setPosition(notes[i].textEnd, QTextCursor::KeepAnchor);
        auto wash = accent;
        wash.setAlpha(i == activeNote ? 72 : notes[i].check->isChecked() ? 50 : 19);
        selection.format.setBackground(wash);
        selections.append(selection);
    }
    transcription->setExtraSelections(selections);
}

void ResultReviewDialog::updateApply()
{
    const auto selected = std::count_if(notes.cbegin(), notes.cend(), [](const Note &note) {
        return note.check->isChecked();
    });
    apply->setEnabled(selected > 0 && endpoint->isEnabled());
    noteCount->setText(notes.isEmpty() ? QString() : tr("%1 of %2 selected").arg(selected).arg(notes.size()));
}
void ResultReviewDialog::setWorking(bool active)
{
    for (auto *field : { endpoint, model, token })
        field->setEnabled(!active);
    withImage->setEnabled(!active && !image.isNull() && image.size() == QSize(page.width, page.height));
    directAnswer->setEnabled(!active);
    ask->setEnabled(!active && batchIndex < batches.size());
    updateApply();
}
void ResultReviewDialog::sendBatch()
{
    if (batchIndex >= batches.size())
        return;
    const auto &batch = batches[batchIndex];
    QRect crop;
    for (const auto &part : batch)
        crop |= page.words[part.word].bounds;
    crop = crop.adjusted(-4, -4, 4, 4).intersected(image.rect());
    QJsonArray items;
    for (const auto &part : batch) {
        const auto &w = page.words[part.word];
        QJsonObject item { { "id", part.key }, { "text", part.text }, { "block", w.block }, { "line", w.line },
            { "paragraph", w.paragraph } };
        if (usedImage) {
            const auto box = w.bounds.translated(-crop.topLeft());
            item["box"] = QJsonArray { box.x(), box.y(), box.width(), box.height() };
        }
        items.append(item);
    }
    QString prompt
        = tr("Review this OCR transcription for likely recognition errors. Preserve the author's language, "
             "spelling, "
             "notation, layout and numbers. Check context and mathematical notation. Omit speculative changes. "
             "Return JSON {\"changes\":[{\"id\":\"source item id\",\"before\":\"exact original substring\","
             "\"after\":\"replacement\",\"reason\":\"brief evidence\"}]}. Within each item, quote enough "
             "original text "
             "to match exactly once. Return an empty changes array if nothing needs correction. ");
    if (usedImage)
        prompt += tr("The attached scan shows a %1 by %2 pixel crop. Boxes are x, y, width, height in those "
                     "original crop coordinates. ")
                      .arg(crop.width())
                      .arg(crop.height());
    if (!nearby.isEmpty())
        prompt += tr("Nearby text for context: %1. ").arg(nearby);
    prompt += QString::fromUtf8(QJsonDocument(items).toJson(QJsonDocument::Compact));
    QByteArray png;
    if (usedImage) {
        if (crop.isEmpty()) {
            status->setText(tr("This selection has no scan coordinates."));
            setWorking(false);
            return;
        }
        auto portion = image.copy(crop);
        if (portion.width() > 1500 || portion.height() > 1500)
            portion = portion.scaled(1500, 1500, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!portion.save(&buffer, "PNG") || png.size() > 4 * 1024 * 1024) {
            status->setText(tr("The crop could not be sent. Try text-only review."));
            setWorking(false);
            return;
        }
    }
    setWorking(true);
    status->setText(tr("Reviewing batch %1 of %2…").arg(batchIndex + 1).arg(batches.size()));
    const auto sent = chat.send(
        usedEndpoint, usedModel, token->text(), prompt, png, { .directAnswer = usedDirectAnswer }, [this](ReviewChat::Answer reply) {
            if (!reply) {
                status->setText(reply.error());
                setWorking(false);
                return;
            }
            auto result = intelligence::readChanges(batches[batchIndex], *reply);
            if (!result) {
                status->setText(result.error());
                setWorking(false);
                return;
            }
            for (auto &change : *result) {
                changes.append(std::move(change));
                appendNote(changes.size() - 1);
            }
            if (!result->isEmpty())
                renderTranscription();
            updateApply();
            ++batchIndex;
            if (batchIndex < batches.size()) {
                sendBatch();
                return;
            }
            setWorking(false);
            status->setText(tr("Review complete."));
            if (changes.isEmpty())
                emptyNotes->setText(tr("The model suggested no changes."));
            else if (activeNote < 0) {
                const auto first = std::ranges::min_element(changes, {}, [](const auto &change) {
                    return std::pair{change.word, change.offset};
                });
                focusNote(first - changes.begin(), true);
            }
        }, [this](qint64 seconds) {
            status->setText(tr("Receiving batch %1 of %2 · %3 s elapsed")
                                .arg(batchIndex + 1).arg(batches.size()).arg(seconds));
        });
    if (!sent) {
        status->setText(sent.error());
        setWorking(false);
        return;
    }
    QSettings settings("YAGF", "YAGF Revival");
    settings.setValue("ai/endpoint", usedEndpoint);
    settings.setValue("ai/model", usedModel);
    settings.setValue("ai/directAnswerFor", usedDirectAnswer ? QStringList{usedEndpoint, usedModel} : QStringList{});
}
}
