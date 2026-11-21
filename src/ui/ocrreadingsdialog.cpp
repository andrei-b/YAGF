#include "ocrreadingsdialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QJsonDocument>
#include <QJsonArray>
#include <QVBoxLayout>
#include <algorithm>

namespace yagf::ui {
OcrReadingsDialog::OcrReadingsDialog(core::OcrPage source,QImage pixels,QWidget *parent):QDialog(parent),page(std::move(source)),image(std::move(pixels)) {
    setObjectName("ocrReadingsDialog"); setWindowTitle(tr("Review OCR readings")); resize(1120,760);
    auto *layout=new QVBoxLayout(this); auto *intro=new QLabel(tr("Compare each primary line or block with independent OCR readings and the source image. Use a reading only after checking it. Save applies your choices; Undo can restore the previous page."),this); intro->setWordWrap(true); layout->addWidget(intro);
    auto *split=new QSplitter(this); lines=new QListWidget(split); lines->setObjectName("ocrReadingLines"); lines->setIconSize({180,58});
    auto *panel=new QWidget(split); auto *review=new QVBoxLayout(panel);
    preview=new QLabel(panel); preview->setAlignment(Qt::AlignCenter); preview->setMinimumSize(420,130); preview->setStyleSheet("background:white;border:1px solid #d7e1ed;border-radius:6px"); review->addWidget(preview);
    review->addWidget(new QLabel(tr("Current primary text"),panel)); primaryText=new QPlainTextEdit(panel); primaryText->setReadOnly(true); primaryText->setMaximumHeight(125); review->addWidget(primaryText);
    engines=new QComboBox(panel); engines->setObjectName("ocrReadingEngine");
    for (const auto &reading:page.ocrReadings) engines->addItem(reading.label+(reading.error.isEmpty() ? QString() : tr(" — failed")));
    review->addWidget(engines); complete=new QCheckBox(tr("Show complete engine output"),panel); review->addWidget(complete);
    alternative=new QPlainTextEdit(panel); alternative->setReadOnly(true); alternative->setObjectName("ocrAlternativeText"); review->addWidget(alternative,1);
    status=new QLabel(panel); status->setTextFormat(Qt::PlainText); status->setWordWrap(true); review->addWidget(status);
    use=new QPushButton(tr("Use this reading for the selected line / block"),panel); use->setObjectName("useOcrReading"); review->addWidget(use); split->setSizes({420,680}); layout->addWidget(split,1);
    reject=new QPushButton(tr("Reject this reading for the selected line / block"),panel);reject->setObjectName("rejectOcrReading");review->addWidget(reject);
    auto *history=new QPushButton(tr("Recognition history…"),this);history->setObjectName("recognitionHistory");history->setEnabled(!page.recognitionHistory.runs.isEmpty());layout->addWidget(history);
    connect(history,&QPushButton::clicked,this,&OcrReadingsDialog::showHistory);
    connect(reject,&QPushButton::clicked,this,&OcrReadingsDialog::rejectReading);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,this,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(lines,&QListWidget::currentRowChanged,this,&OcrReadingsDialog::showSpan); connect(engines,&QComboBox::currentIndexChanged,this,&OcrReadingsDialog::showReading); connect(complete,&QCheckBox::toggled,this,&OcrReadingsDialog::showReading); connect(use,&QPushButton::clicked,this,&OcrReadingsDialog::useReading);
    rebuild();
}
void OcrReadingsDialog::rebuild(int row) {
    const QSignalBlocker block(lines); spans=core::ocrReadingSpans(page); lines->clear();
    for (const auto &span:spans) {
        QRect bounds; QStringList text; for (int i:span) {bounds|=page.words[i].bounds; text.append(page.words[i].text);}
        auto *item=new QListWidgetItem(QIcon(QPixmap::fromImage(image.copy(bounds).scaled(180,58,Qt::KeepAspectRatio,Qt::SmoothTransformation))),text.join(' ').left(160),lines); item->setSizeHint({360,78});
    }
    if (spans.isEmpty()) {lines->addItem(tr("No primary text — view complete engine output")); complete->setChecked(true);}
    lines->setCurrentRow(std::clamp(row,0,std::max(0,int(spans.size())-1))); showSpan();
}
void OcrReadingsDialog::showSpan() {
    const int row=lines->currentRow(); QRect bounds; QStringList text;
    if (row>=0 && row<spans.size()) for (int i:spans[row]) {bounds|=page.words[i].bounds; text.append(page.words[i].text);}
    primaryText->setPlainText(text.join(' '));
    const auto crop=bounds.isEmpty() ? image : image.copy(bounds.adjusted(-6,-6,6,6).intersected(image.rect()));
    preview->setPixmap(QPixmap::fromImage(crop.scaled(preview->size()-QSize(16,16),Qt::KeepAspectRatio,Qt::SmoothTransformation))); showReading();
}
void OcrReadingsDialog::showReading() {
    matching.clear(); const int index=engines->currentIndex(),row=lines->currentRow(); use->setEnabled(false);reject->setEnabled(false);
    if (index<0 || index>=page.ocrReadings.size()) {alternative->clear(); return;}
    const auto &reading=page.ocrReadings[index];
    if (row>=0 && row<spans.size()) matching=core::matchedOcrReading(page,spans[row],reading);
    alternative->setPlainText(complete->isChecked() ? core::ocrReadingText(reading) : matching);
    if (!reading.error.isEmpty()) status->setText(reading.error+tr(" The primary reading was retained."));
    else if (matching.isEmpty()) status->setText(tr("No safe match for this line. View the complete output to inspect unmatched or larger blocks."));
    else if (matching.simplified()==primaryText->toPlainText().simplified()) status->setText(tr("The engines agree on this reading."));
    else status->setText(tr("The readings differ. Compare both with the image before choosing."));
    reject->setEnabled(!complete->isChecked() && !matching.isEmpty() && !reading.historyIds.isEmpty() && reading.error.isEmpty());
    use->setEnabled(!complete->isChecked() && !matching.isEmpty() && matching.size()<=16384 && reading.error.isEmpty());
}
void OcrReadingsDialog::useReading() {
    const int row=lines->currentRow(),index=engines->currentIndex();
    if (row<0 || row>=spans.size() || index<0 || index>=page.ocrReadings.size() || !use->isEnabled()) return;
    auto result=core::acceptOcrReading(page,spans[row],page.ocrReadings[index],matching);
    if (!result) {status->setText(result.error()); return;}
    page=std::move(*result); changed=true; rebuild(row);
}
void OcrReadingsDialog::rejectReading() {
    const int row=lines->currentRow(),index=engines->currentIndex();
    if(!reject->isEnabled() || row<0 || row>=spans.size() || index<0 || index>=page.ocrReadings.size())return;
    const auto count=page.recognitionHistory.decisions.size();
    core::recordReadingDecision(page,page,spans[row],page.ocrReadings[index],matching,"rejected");
    if(page.recognitionHistory.decisions.size()>count){changed=true;status->setText(tr("Rejection recorded. Save applies this decision."));}
}
void OcrReadingsDialog::showHistory() {
    QDialog dialog(this);dialog.setObjectName("recognitionHistoryDialog");dialog.setWindowTitle(tr("Recognition history"));dialog.resize(1050,740);
    auto *layout=new QVBoxLayout(&dialog);auto *intro=new QLabel(tr("Original model inputs, results and review decisions. Missing versions and hashes were not reported by the engine. Coordinates describe the frames used at recognition time."),&dialog);intro->setWordWrap(true);layout->addWidget(intro);
    auto *split=new QSplitter(&dialog);auto *runs=new QListWidget(split);runs->setObjectName("recognitionRuns");auto *tabs=new QTabWidget(split);
    auto *crop=new QLabel;crop->setAlignment(Qt::AlignCenter);crop->setMinimumSize(360,250);tabs->addTab(crop,tr("Source crop"));
    auto editor=[&](const QString &title){auto *text=new QPlainTextEdit;text->setReadOnly(true);tabs->addTab(text,title);return text;};
    auto *metadata=editor(tr("Run details")),*output=editor(tr("Full output")),*frames=editor(tr("Preprocessing / coordinates")),*decisions=editor(tr("Decisions"));
    output->setObjectName("recognitionRawOutput");metadata->setObjectName("recognitionRunDetails");
    for(const auto &run:page.recognitionHistory.runs)runs->addItem(QStringLiteral("%1 • %2 • %3\n%4").arg(run.engine,run.role,run.status,run.startedUtc));
    connect(runs,&QListWidget::currentRowChanged,&dialog,[&](int row){
        if(row<0 || row>=page.recognitionHistory.runs.size())return;
        const auto &run=page.recognitionHistory.runs[row];
        const auto pixels=QImage::fromData(run.inputPng,"PNG");
        if(pixels.isNull()){crop->setPixmap({});crop->setText(tr("No input image was recorded."));}
        else crop->setPixmap(QPixmap::fromImage(pixels.scaled(650,500,Qt::KeepAspectRatio,Qt::SmoothTransformation)));
        core::RecognitionHistory one;one.runs.append(run);one.runs[0].inputPng.clear();
        auto value=core::recognitionHistoryJson(one)["runs"].toArray().first().toObject();
        value.remove("output");value.remove("preprocessing");value.remove("source_to_input");value.remove("source_crop");
        metadata->setPlainText(QString::fromUtf8(QJsonDocument(value).toJson()));
        output->setPlainText(QString::fromUtf8(QJsonDocument(run.output).toJson()));
        frames->setPlainText(QString::fromUtf8(QJsonDocument(QJsonObject{{"preprocessing",run.preprocessing},{"source_to_input",run.sourceToInput.toJson()},
            {"source_crop",core::recognitionHistoryJson(one)["runs"].toArray().first().toObject()["source_crop"]}}).toJson()));
        QJsonArray events;core::RecognitionHistory decisionsOnly;decisionsOnly.decisions=page.recognitionHistory.decisions;auto saved=core::recognitionHistoryJson(decisionsOnly);for(const auto &d:saved["decisions"].toArray())if(d.toObject()["run_id"]==run.id)events.append(d);
        decisions->setPlainText(QString::fromUtf8(QJsonDocument(events).toJson()));
    });
    split->setSizes({320,710});layout->addWidget(split,1);auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close,&dialog);layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    runs->setCurrentRow(runs->count()-1);dialog.exec();
}

}
