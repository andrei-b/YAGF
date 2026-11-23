#include "inlineformuladialog.h"
#include "formulaeditor.h"
#include "formulagrowth.h"
#include <QComboBox>
#include <QCheckBox>
#include <algorithm>
#include "formulatext.h"
#include "recognitionrecorder.h"

#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QGraphicsRectItem>
#include <QGraphicsView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QUuid>
#include <QTimer>
#include <QtConcurrentRun>

namespace yagf::ui {

namespace {
QStringList tableHeaders(const QString &text, const QJsonArray &layout, int position)
{
    const auto prefix = QStringView(text).left(position);
    const int targetRow = prefix.count('\n');
    const int targetCell = prefix.mid(prefix.lastIndexOf('\n')+1).count('\t');
    const auto rows = text.split('\n');
    struct Header { QRect cells; QString label; };
    QVector<Header> headers;
    QVector<int> occupied(64);
    for (int r=0; r<=targetRow && r<layout.size() && r<rows.size(); ++r) {
        const auto cells = rows[r].split('\t');
        const auto formats = layout[r].toArray();
        int col = 0;
        for (int i=0; i<cells.size() && i<formats.size(); ++i) {
            while (col<occupied.size() && occupied[col]>r) ++col;
            const auto meta = formats[i].toObject();
            const QRect area(col,r,meta["colspan"].toInt(1),meta["rowspan"].toInt(1));
            if (area.right() >= occupied.size() || area.isEmpty()) return {};
            if (r == targetRow && i == targetCell) {
                QStringList labels;
                for (const auto &header : headers)
                    if ((header.cells.bottom() < r && header.cells.left() <= area.right() && header.cells.right() >= col) ||
                        (header.cells.contains(header.cells.left(), r) && header.cells.right() < col))
                        labels.append(header.label);
                return labels;
            }
            if (meta["header"].toBool() && !cells[i].isEmpty()) headers.append({area,cells[i]});
            for (;col<=area.right();++col) occupied[col] = area.bottom()+1;
        }
    }
    return {};
}
}

std::optional<core::OcrPage> markInlineFormula(QWidget *parent, const core::OcrPage &page,
    int wordIndex, int start, int length, const QImage &image, QRect crop,
    const processing::OcrService &service, const QString &path, bool unicode)
{
    const auto &word = page.words.at(wordIndex);
    const auto original = core::formulaSourceText(word);
    crop = crop.intersected(image.rect());
    if (crop.isEmpty()) return {};
    QRect paragraph;
    for (const auto &other : page.words)
        if (other.block == word.block && other.paragraph == word.paragraph) paragraph |= other.bounds;
    const core::OcrRegion *table = nullptr;
    for (const auto &region : page.regions) if (region.type == "table")
        for (const auto &para : region.paragraphs) for (const auto &line : para.lines)
            if (line.wordIds.contains(word.id)) table = &region;
    QDialog dialog(parent);
    dialog.setObjectName("inlineFormulaTarget");
    dialog.setWindowTitle(QObject::tr("Mark as inline formula"));
    dialog.resize(800, 540);
    QVBoxLayout layout(&dialog);
    layout.addWidget(new QLabel(QObject::tr("Select the exact characters to replace.")));
    QPlainTextEdit passage(original);
    passage.setReadOnly(true);
    passage.setObjectName("inlineFormulaPassage");
    QTextCursor cursor = passage.textCursor();
    if (start >= 0 && length > 0 && start <= original.size() - length) {
        cursor.setPosition(start);
        cursor.setPosition(start + length, QTextCursor::KeepAnchor);
        passage.setTextCursor(cursor);
    }
    layout.addWidget(&passage);
    QLabel cellContext;
    cellContext.setTextFormat(Qt::PlainText);
    layout.addWidget(&cellContext);
    const auto updateHeaders = [&] {
        const auto headers = table ? tableHeaders(original, table->tableLayout, passage.textCursor().selectionStart()) : QStringList{};
        cellContext.setText(QObject::tr("Cell headers: %1").arg(headers.join(", ")));
        cellContext.setVisible(!headers.isEmpty());
    };
    QObject::connect(&passage, &QPlainTextEdit::cursorPositionChanged, &dialog, updateHeaders);
    updateHeaders();
    layout.addWidget(new QLabel(QObject::tr("Drag over the expression in the scan to adjust its crop.")));
    QGraphicsScene scene;
    scene.addPixmap(QPixmap::fromImage(image));
    auto *outline = scene.addRect(crop, QPen(Qt::red, 0));
    QGraphicsView view(&scene);
    view.setDragMode(QGraphicsView::RubberBandDrag);
    layout.addWidget(&view, 3);
    QObject::connect(&view, &QGraphicsView::rubberBandChanged, &dialog,
        [&](const QRect &band, QPointF first, QPointF last) {
            if (band.isEmpty()) return;
            const auto box = QRectF(first, last).normalized().toAlignedRect().intersected(image.rect());
            if (!box.isEmpty()) { crop = box; outline->setRect(crop); }
        });
    QTimer::singleShot(0, &dialog, [&] {
        const int margin = qMax(24, crop.height()/2);
        const auto vicinity = crop.united(word.bounds).adjusted(-margin,-margin,margin,margin);
        view.fitInView(vicinity.intersected(image.rect()), Qt::KeepAspectRatio);
    });
    QLabel status;
    status.setWordWrap(true);
    status.setTextFormat(Qt::PlainText);
    layout.addWidget(&status);
    QRect evidenceWindow;
    QPushButton widen(QObject::tr("Check crop for detached limits"));
    layout.addWidget(&widen);
    QObject::connect(&widen, &QPushButton::clicked, &dialog, [&] {
        const auto gray = image.convertToFormat(QImage::Format_Grayscale8);
        const int halo = std::clamp(crop.height(), 10, 120);
        evidenceWindow = crop.adjusted(-halo,-halo,halo,halo).intersected(image.rect());
        QVector<QRect> protectedWords;
        for (const auto &other : page.words)
            if (!other.bounds.intersects(crop) && !other.layoutBlock && other.bounds.intersects(evidenceWindow))
                protectedWords.append(other.bounds);
        auto grown = processing::growSymbolCrop(gray,crop,evidenceWindow,protectedWords,false,{});
        for (const auto &sign : processing::boundedIntegralSigns(gray,evidenceWindow))
            if (crop.contains(sign.center())) grown |= sign;
        if (grown == crop) status.setText(QObject::tr("The crop already covers the detected ink."));
        crop = grown;
        outline->setRect(crop);
        view.ensureVisible(crop);
    });
    QCheckBox experimental(QObject::tr("Show experimental formula and symbol suggestions"));
    layout.addWidget(&experimental);
    QPushButton find(QObject::tr("Find missed formulas"));
    QComboBox proposals;
    proposals.hide();
    find.hide();
    layout.addWidget(&find);
    layout.addWidget(&proposals);
    QFutureWatcher<QVector<QRect>> search;
    std::stop_source stop;
    QObject::connect(&find, &QPushButton::clicked, &dialog, [&] {
        find.setEnabled(false);
        search.setFuture(QtConcurrent::run([image, area=paragraph, token=stop.get_token()] {
            return processing::paragraphFormulaWindows(image.convertToFormat(QImage::Format_Grayscale8), area, token);
        }));
    });
    QObject::connect(&search, &QFutureWatcherBase::finished, &dialog, [&] {
        proposals.clear();
        proposals.addItem(QObject::tr("Choose a proposed crop"));
        for (const auto &box : search.result())
            proposals.addItem(QObject::tr("Expression %1 (%2, %3)").arg(proposals.count()).arg(box.x()).arg(box.y()), box);
        proposals.setVisible(experimental.isChecked() && proposals.count() > 1);
        if (proposals.count()==1) status.setText(QObject::tr("No formula candidates found in this area."));
    });
    QObject::connect(&proposals, &QComboBox::activated, &dialog, [&](int i) {
        if (i <= 0) return;
        evidenceWindow = paragraph;
        crop = proposals.itemData(i).toRect();
        outline->setRect(crop);
        view.ensureVisible(crop);
    });
    QComboBox symbols;
    layout.addWidget(&symbols);
    const auto updateChoices = [&] {
        const auto selected = passage.textCursor();
        symbols.clear();
        symbols.addItem(QObject::tr("Review symbol readings"));
        if (!experimental.isChecked()) { symbols.hide(); return; }
        for (const auto &choice : core::localSymbolChoices(page, wordIndex, selected.selectionStart(),
                                                      selected.selectionEnd()-selected.selectionStart()))
            symbols.addItem(choice.source + " (" + (choice.source[0].script() == QChar::Script_Latin ? QObject::tr("Latin") :
                  choice.source[0].script() == QChar::Script_Cyrillic ? QObject::tr("Cyrillic") : QObject::tr("Greek")) + ")", choice.source);
        symbols.setVisible(experimental.isChecked() && symbols.count() > 1);
    };
    QObject::connect(&passage, &QPlainTextEdit::selectionChanged, &dialog, updateChoices);
    QObject::connect(&experimental, &QCheckBox::toggled, &dialog, [&](bool checked) {
        find.setVisible(checked);
        proposals.setVisible(checked && proposals.count() > 1);
        updateChoices();
    });
    updateChoices();
    QDialogButtonBox buttons(QDialogButtonBox::Cancel);
    auto *recognize = buttons.addButton(QObject::tr("Recognize crop"), QDialogButtonBox::ActionRole);
    auto *manual = buttons.addButton(QObject::tr("Enter LaTeX…"), QDialogButtonBox::ActionRole);
    layout.addWidget(&buttons);
    core::RecognitionHistory history;
    QString replacement;
    int offset = 0, count = 0;
    bool insideFormula = false;
    QString initial;
    const auto selectSpan = [&] {
        const auto selected = passage.textCursor();
        offset = selected.selectionStart();
        count = selected.selectionEnd() - offset;
        if (count == 0) { status.setText(QObject::tr("Select the characters in the paragraph first.")); return false; }
        initial = original.mid(offset, count);
        if (table && (initial.contains('\t') || initial.contains('\n'))) {
            status.setText(QObject::tr("Select text within one table cell.")); return false;
        }
        insideFormula = false;
        for (const auto &span : core::mathTextSpans(original)) {
            if (offset >= span.start + span.length || offset + count <= span.start) continue;
            if (offset == span.start && count == span.length) { initial = span.source; continue; }
            const auto delimiter = (span.length - span.source.size()) / 2;
            if (offset >= span.start + delimiter && offset + count <= span.start + span.length - delimiter) {
                insideFormula = true; continue;
            }
            status.setText(QObject::tr("Select the formula body or include both delimiters.")); return false;
        }
        return true;
    };
    const auto targetSettings = [&] {
        auto settings = core::preprocessingEvidence(page.preprocessing, page.renderDpi);
        settings["word_id"] = word.id;
        settings["text_offset"] = offset;
        settings["text_length"] = count;
        if (evidenceWindow.isValid()) settings["search_area"] = QJsonArray{
            evidenceWindow.x(),evidenceWindow.y(),evidenceWindow.width(),evidenceWindow.height()};
        return settings;
    };
    const auto review = [&](const QString &candidate) {
        const auto accepted = editFormulaSource(&dialog, candidate, false);
        if (!accepted) return;
        if ((table && (accepted->contains('\t') || accepted->contains('\n'))) ||
            !core::mathTextSpans(*accepted).isEmpty()) {
            status.setText(QObject::tr("Enter one formula body without surrounding delimiters.")); return;
        }
        replacement = *accepted;
        dialog.accept();
    };
    QObject::connect(&symbols, &QComboBox::activated, &dialog, [&](int i) {
        if (i > 0 && selectSpan()) { history = {}; review(symbols.itemData(i).toString()); }
    });
    QObject::connect(manual, &QPushButton::clicked, &dialog, [&] {
        if (selectSpan()) { history = {}; review(initial); }
    });
    using Reading = std::expected<core::OcrPage, processing::OcrError>;
    QFutureWatcher<Reading> watcher;
    QObject::connect(recognize, &QPushButton::clicked, &dialog, [&] {
        if (!selectSpan()) return;
        recognize->setEnabled(false);
        widen.setEnabled(false);
        manual->setEnabled(false);
        passage.setEnabled(false);
        view.setEnabled(false);
        proposals.setEnabled(false);
        symbols.setEnabled(false);
        status.setText(QObject::tr("Recognizing the selected crop…"));
        processing::OcrOptions options;
        options.language = service.language.isEmpty() ? QStringLiteral("auto") : service.language;
        options.pageNumber = page.number;
        options.formula = true;
        options.task = "formula";
        options.historyRole = "formula";
        options.historyCoordinates = page.coordinates;
        const auto settings = targetSettings();
        options = processing::croppedOcrOptions(options, crop);
        watcher.setFuture(QtConcurrent::run([service, path, settings, options, input=image.copy(crop),
                                              token=stop.get_token()]() mutable -> Reading {
            auto engine = processing::makeOcrServiceEngine(service);
            if (!engine) return std::unexpected(processing::OcrError{engine.error()});
            options.history = std::make_shared<processing::RecognitionRecorder>(
                processing::recognitionSourceHash(path, token), settings);
            return (*engine)->recognize(input, options, token);
        }));
    });
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &dialog, [&] {
        recognize->setEnabled(true);
        widen.setEnabled(true);
        manual->setEnabled(true);
        passage.setEnabled(true);
        view.setEnabled(true);
        proposals.setEnabled(true);
        symbols.setEnabled(true);
        const auto result = watcher.result();
        if (!result) { status.setText(result.error().message); return; }
        QStringList pieces;
        for (const auto &item : result->words) pieces.append(core::formulaSourceText(item));
        auto candidate = pieces.join(' ').trimmed();
        if (candidate.isEmpty()) { status.setText(QObject::tr("No formula was recognized.")); return; }
        const auto spans = core::mathTextSpans(candidate);
        if (spans.size() == 1 && spans.first().length == candidate.size()) candidate = spans.first().source;
        history = result->recognitionHistory;
        status.clear();
        review(candidate);
    });
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    stop.request_stop();
    if (!accepted) return {};
    auto edited = page;
    auto &target = edited.words[wordIndex];
    target.text = original;
    target.text.replace(offset, count, insideFormula ? replacement : QStringLiteral("\\(") + replacement + QStringLiteral("\\)"));
    target.latexText.clear();
    target.reviewed = true;
    target.alternative = {};
    target.readingEngine.clear();
    if (history.runs.isEmpty()) {
        const auto settings = targetSettings();
        processing::RecognitionRecorder recorder(processing::recognitionSourceHash(path, {}), settings);
        processing::OcrOptions options;
        options.pageNumber = page.number;
        options.historyCoordinates = page.coordinates;
        options.historyRole = "manual";
        auto run = recorder.begin("user", "manual inline formula", image.copy(crop),
                                  processing::croppedOcrOptions(options, crop));
        QJsonArray alternatives;
        for (int i=1; i<symbols.count(); ++i) alternatives.append(symbols.itemData(i).toString());
        recorder.finish(std::move(run), 0, "succeeded", {}, {{"latex", replacement}, {"alternatives", alternatives}});
        history = recorder.snapshot();
    }
    {
        core::RecognitionDecision decision;
        decision.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        decision.runId = history.runs.last().id;
        decision.timestampUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        decision.decision = "accepted";
        decision.primaryText = original.mid(offset, count);
        decision.proposedText = replacement;
        decision.wordIds = {word.id};
        decision.processedArea = crop;
        decision.sourceToProcessed = page.coordinates;
        history.decisions.append(decision);
        core::mergeRecognitionHistory(edited.recognitionHistory, history);
    }
    if (unicode) {
        const auto rendered = core::unicodeFormulaText(target.text);
        if (rendered.text != target.text) { target.latexText = target.text; target.text = rendered.text; }
    }
    return edited;
}
}
