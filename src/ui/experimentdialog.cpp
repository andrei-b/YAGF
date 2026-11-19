#include "experimentdialog.h"
#include "exportpage.h"
#include "wordeditor.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QUuid>
#include <QJsonDocument>
#include <QtConcurrentRun>
#include <atomic>

namespace yagf::ui {
namespace {
QComboBox *splitBox(QTableWidget *table,int row) {return qobject_cast<QComboBox *>(table->cellWidget(row,2));}
QString percentage(const QJsonValue &value) {return value.isDouble()?QString::number(value.toDouble()*100,'f',2)+"%":QStringLiteral("—");}
}
ExperimentDialog::ExperimentDialog(core::OcrDocument snapshot,QWidget *parent):QDialog(parent),document(std::move(snapshot))
{
    setObjectName(QStringLiteral("experimentManager"));setWindowTitle(tr("OCR experiments"));resize(1280,900);
    auto *layout=new QVBoxLayout(this);
    auto *intro=new QLabel(tr("Compare pipelines on manually verified reference pages. Calibration pages choose the advisory recommendation; separate held-out pages show how it generalizes. Experiments keep their own copies and never replace your project’s text. Remote pipelines send page images to their configured endpoints."));intro->setWordWrap(true);layout->addWidget(intro);
    setup=new QWidget;auto *settings=new QVBoxLayout(setup);settings->setContentsMargins(0,0,0,0);
    auto *splitter=new QSplitter(Qt::Vertical);
    references=new QTableWidget(0,3);references->setObjectName("experimentReferences");references->setHorizontalHeaderLabels({tr("Page"),tr("Reference status"),tr("Dataset split")});
    references->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);references->setSelectionBehavior(QAbstractItemView::SelectRows);references->setEditTriggers(QAbstractItemView::NoEditTriggers);
    splitter->addWidget(references);
    pipelines=new QTableWidget(0,12);pipelines->setObjectName("experimentPipelines");pipelines->setHorizontalHeaderLabels({tr("Use / label"),tr("Provider"),tr("Endpoint"),tr("Token (session only)"),tr("Language"),tr("DPI"),tr("Binarize"),tr("Auto rotation/skew"),tr("ID"),tr("Saved filters"),tr("Contrast"),tr("Denoise")});pipelines->hideColumn(8);pipelines->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);splitter->addWidget(pipelines);settings->addWidget(splitter);
    auto *actions=new QHBoxLayout;auto *verify=new QPushButton(tr("Proofread reference…"));verify->setObjectName("proofreadExperimentReference");
    auto *add=new QPushButton(tr("Add pipeline")),*remove=new QPushButton(tr("Remove pipeline"));
    actions->addWidget(verify);actions->addStretch();actions->addWidget(add);actions->addWidget(remove);settings->addLayout(actions);
    repetitions=new QSpinBox;repetitions->setRange(1,10);repetitions->setValue(2);repetitions->setObjectName("experimentRepetitions");
    warmups=new QSpinBox;warmups->setRange(0,3);warmups->setValue(1);warmups->setObjectName("experimentWarmups");
    auto *counts=new QHBoxLayout;counts->addWidget(new QLabel(tr("Measured repetitions:")));counts->addWidget(repetitions);counts->addWidget(new QLabel(tr("Calibration warmups per pipeline:")));counts->addWidget(warmups);counts->addStretch();settings->addLayout(counts);layout->addWidget(setup);
    results=new QTableWidget(0,9);results->setObjectName("experimentResults");results->setHorizontalHeaderLabels({tr("Pipeline"),tr("Calibration CER"),tr("Held-out CER"),tr("Held-out WER"),tr("Mean ms"),tr("p95 ms"),tr("Pages/min"),tr("Peak RSS MiB"),tr("Status")});results->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);results->setSelectionBehavior(QAbstractItemView::SelectRows);results->setEditTriggers(QAbstractItemView::NoEditTriggers);layout->addWidget(results);
    status=new QLabel(tr("Choose at least one verified calibration page and one different held-out page."));status->setObjectName("experimentStatus");status->setTextFormat(Qt::PlainText);status->setWordWrap(true);layout->addWidget(status);
    progress=new QProgressBar;progress->setRange(0,1);progress->setValue(0);layout->addWidget(progress);
    auto *buttons=new QHBoxLayout;run=new QPushButton(tr("Run experiments"));run->setObjectName("runExperiments");cancel=new QPushButton(tr("Cancel run"));cancel->setEnabled(false);save=new QPushButton(tr("Save report…"));save->setEnabled(false);inspect=new QPushButton(tr("Inspect readings…"));inspect->setEnabled(false);auto *open=new QPushButton(tr("Open report…"));auto *close=new QPushButton(tr("Close"));buttons->addWidget(open);buttons->addWidget(run);buttons->addWidget(cancel);buttons->addWidget(save);buttons->addWidget(inspect);buttons->addStretch();buttons->addWidget(close);layout->addLayout(buttons);
    refreshReferences();addPipeline(tr("Saved preprocessing / DPI"),"tesseract",0,false,true);addPipeline(tr("300 DPI + local binarization + deskew"),"tesseract",300,true,true);addPipeline(tr("PaddleOCR 300 DPI"),"paddleocr",300,true,false);
    connect(verify,&QPushButton::clicked,this,&ExperimentDialog::proofreadReference);
    connect(add,&QPushButton::clicked,this,[this]{if(pipelines->rowCount()<16)addPipeline(tr("New pipeline"),"tesseract",0,false,true);});
    connect(remove,&QPushButton::clicked,this,[this]{if(pipelines->currentRow()>=0)pipelines->removeRow(pipelines->currentRow());});
    connect(run,&QPushButton::clicked,this,&ExperimentDialog::start);connect(cancel,&QPushButton::clicked,this,[this]{stop.request_stop();status->setText(tr("Cancelling… completed runs can still be saved."));});
    connect(open,&QPushButton::clicked,this,&ExperimentDialog::openReport);
    connect(save,&QPushButton::clicked,this,&ExperimentDialog::saveReport);connect(inspect,&QPushButton::clicked,this,&ExperimentDialog::inspectReading);connect(close,&QPushButton::clicked,this,&ExperimentDialog::reject);
    connect(this,&ExperimentDialog::progressChanged,this,[this](int done,int total,const QString &text){progress->setRange(0,total);progress->setValue(done);status->setText(text);},Qt::QueuedConnection);
    connect(&watcher,&QFutureWatcherBase::finished,this,[this]{
        setup->setEnabled(true);run->setEnabled(true);cancel->setEnabled(false);
        const auto outcome=watcher.result();if(!outcome){status->setText(outcome.error());return;}
        report=*outcome;showResults();save->setEnabled(true);inspect->setEnabled(true);
    });
}
ExperimentDialog::~ExperimentDialog(){stop.request_stop();if(watcher.isRunning())watcher.waitForFinished();}
void ExperimentDialog::reject(){if(watcher.isRunning()){stop.request_stop();status->setText(tr("Cancelling… close again after the run stops."));return;}QDialog::reject();}
void ExperimentDialog::addPipeline(const QString &label,const QString &provider,int dpi,bool enhance,bool checked)
{
    const int row=pipelines->rowCount();pipelines->insertRow(row);auto *name=new QTableWidgetItem(label);name->setCheckState(checked?Qt::Checked:Qt::Unchecked);pipelines->setItem(row,0,name);
    auto *kind=new QComboBox;kind->addItems({"tesseract","paddleocr","doctr","rapidocr","surya","kraken"});kind->setCurrentText(provider);pipelines->setCellWidget(row,1,kind);
    auto *endpoint=new QLineEdit(provider=="tesseract"?QString():QStringLiteral("http://127.0.0.1:8765"));endpoint->setMinimumWidth(150);pipelines->setCellWidget(row,2,endpoint);
    auto *token=new QLineEdit;token->setEchoMode(QLineEdit::Password);pipelines->setCellWidget(row,3,token);
    auto *language=new QLineEdit(provider=="tesseract"?(document.language.isEmpty() || document.language=="auto"?QStringLiteral("eng"):document.language):QStringLiteral("auto"));language->setMaximumWidth(90);pipelines->setCellWidget(row,4,language);
    auto *resolution=new QComboBox;resolution->addItem(tr("Saved"),0);resolution->addItem(tr("Adaptive"),-1);for(int n:{150,200,300,400})resolution->addItem(QString::number(n),n);resolution->setCurrentIndex(resolution->findData(dpi));pipelines->setCellWidget(row,5,resolution);
    auto *binary=new QCheckBox;binary->setChecked(enhance);pipelines->setCellWidget(row,6,binary);auto *angle=new QCheckBox;angle->setChecked(enhance);pipelines->setCellWidget(row,7,angle);
    auto *savedFilters=new QCheckBox;savedFilters->setChecked(true);savedFilters->setToolTip(tr("Keep saved contrast, denoise, and binarization settings. Clear to compare from the source colors."));pipelines->setCellWidget(row,9,savedFilters);
    for(int col:{10,11})pipelines->setCellWidget(row,col,new QCheckBox);
    pipelines->setItem(row,8,new QTableWidgetItem(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    connect(kind,&QComboBox::currentTextChanged,this,[endpoint,language](const QString &value){language->setText(value=="tesseract"?"eng":"auto");if(value!="tesseract" && endpoint->text().isEmpty())endpoint->setText("http://127.0.0.1:8765");});
}
void ExperimentDialog::refreshReferences()
{
    QHash<int,int> old;for(int row=0;row<references->rowCount();++row)old.insert(references->item(row,0)->data(Qt::UserRole).toInt(),splitBox(references,row)->currentIndex());
    references->setRowCount(0);int eligible=0;
    for(const auto &page:document.pages) {
        const int row=references->rowCount();references->insertRow(row);auto *number=new QTableWidgetItem(QString::number(page.number));number->setData(Qt::UserRole,page.number);references->setItem(row,0,number);
        const QString problem=core::benchmarkReferenceProblem(page);references->setItem(row,1,new QTableWidgetItem(problem.isEmpty()?tr("Verified · %1 OCR items").arg(page.words.size()):problem));
        auto *split=new QComboBox;split->addItems({tr("Exclude"),tr("Calibration"),tr("Held out")});split->setEnabled(problem.isEmpty());
        if(problem.isEmpty()){++eligible;split->setCurrentIndex(old.contains(page.number)?old[page.number]:(eligible==1?1:eligible==2?2:0));}
        references->setCellWidget(row,2,split);
    }
}
void ExperimentDialog::proofreadReference()
{
    const int row=references->currentRow();if(row<0){status->setText(tr("Select a reference page first."));return;}
    const int number=references->item(row,0)->data(Qt::UserRole).toInt();const auto *page=document.page(number);if(!page || page->words.isEmpty())return;
    if(!page->pendingCorrections.isEmpty()){status->setText(tr("Resolve this page’s saved corrections in the project before verifying a reference."));return;}
    std::atomic_bool cancelled=false;QTemporaryDir temporary;auto image=processing::readExportPage(document,number,temporary,cancelled);if(!image){status->setText(image.error().error);return;}
    QDialog dialog(this);dialog.setWindowTitle(tr("Proofread reference page %1").arg(number));dialog.resize(1150,850);auto *layout=new QVBoxLayout(&dialog);
    auto *notice=new QLabel(tr("Check every passage against the image. Confirming marks this experiment’s reference copy as manually verified; it does not change your project. Do not confirm uncorrected OCR as ground truth."));notice->setWordWrap(true);layout->addWidget(notice);
    auto *split=new QSplitter;QGraphicsScene scene;scene.addPixmap(QPixmap::fromImage(*image));auto *view=new QGraphicsView(&scene);view->setDragMode(QGraphicsView::ScrollHandDrag);split->addWidget(view);
    core::OcrDocument one;one.pages={*page};const QString initial=one.plainText();auto *text=new QPlainTextEdit(initial);text->setObjectName("experimentReferenceText");split->addWidget(text);layout->addWidget(split);
    auto *zoom=new QSlider(Qt::Horizontal);zoom->setRange(20,200);zoom->setValue(50);view->scale(.5,.5);connect(zoom,&QSlider::valueChanged,&dialog,[view](int n){view->resetTransform();view->scale(n/100.0,n/100.0);});layout->addWidget(zoom);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Ok)->setText(tr("Confirm verified reference"));layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted)return;
    auto verified=*page;
    if(text->toPlainText().trimmed().isEmpty()){status->setText(tr("A text reference must not be empty."));return;}
    if(text->toPlainText()!=initial) {
        if(verified.words.size()==1 && verified.words[0].layoutBlock)verified.words[0].text=text->toPlainText();
        else {QVector<int> all;for(int i=0;i<verified.words.size();++i)all.append(i);auto edited=core::WordEditor::acceptPassage(verified,all,text->toPlainText());if(!edited){status->setText(edited.error());return;}verified=std::move(*edited);}
    }
    for(auto &word:verified.words){word.reviewed=true;word.alternative={};}
    document.replacePage(std::move(verified));refreshReferences();
    auto *splitChoice=splitBox(references,row);if(splitChoice->currentIndex()==0){bool calibration=false;for(int i=0;i<references->rowCount();++i)calibration|=splitBox(references,i)->currentIndex()==1;splitChoice->setCurrentIndex(calibration?2:1);}
}
void ExperimentDialog::start()
{
    experiments::ExperimentPlan plan;plan.repetitions=repetitions->value();plan.warmups=warmups->value();
    for(int row=0;row<references->rowCount();++row){const int which=splitBox(references,row)->currentIndex();if(which)plan.references.append({references->item(row,0)->data(Qt::UserRole).toInt(),which==2});}
    for(int row=0;row<pipelines->rowCount();++row) {
        if(pipelines->item(row,0)->checkState()!=Qt::Checked)continue;
        experiments::Pipeline p;p.id=pipelines->item(row,8)->text();p.label=pipelines->item(row,0)->text();p.provider=qobject_cast<QComboBox *>(pipelines->cellWidget(row,1))->currentText();
        p.endpoint=QUrl(qobject_cast<QLineEdit *>(pipelines->cellWidget(row,2))->text().trimmed());p.token=qobject_cast<QLineEdit *>(pipelines->cellWidget(row,3))->text();p.language=qobject_cast<QLineEdit *>(pipelines->cellWidget(row,4))->text().trimmed();p.dpi=qobject_cast<QComboBox *>(pipelines->cellWidget(row,5))->currentData().toInt();p.binarize=qobject_cast<QCheckBox *>(pipelines->cellWidget(row,6))->isChecked();p.autoDeskew=qobject_cast<QCheckBox *>(pipelines->cellWidget(row,7))->isChecked();p.savedFilters=qobject_cast<QCheckBox *>(pipelines->cellWidget(row,9))->isChecked();p.contrast=qobject_cast<QCheckBox *>(pipelines->cellWidget(row,10))->isChecked();p.denoise=qobject_cast<QCheckBox *>(pipelines->cellWidget(row,11))->isChecked();plan.pipelines.append(p);
    }
    stop=std::stop_source{};const auto token=stop.get_token();const auto snapshot=document;
    setup->setEnabled(false);run->setEnabled(false);cancel->setEnabled(true);status->setText(tr("Preparing experiments…"));
    watcher.setFuture(QtConcurrent::run([this,snapshot,plan,token]{return experiments::runExperiment(snapshot,plan,token,[this](int n,int total,const QString &message){emit progressChanged(n,total,message);});}));
}
void ExperimentDialog::showResults()
{
    results->setRowCount(0);for(const auto &value:report["summary"].toArray()) {
        const auto row=value.toObject();const int i=results->rowCount();results->insertRow(i);const auto cal=row["calibration"].toObject(),hold=row["held_out"].toObject();
        const QStringList cells{row["label"].toString(),percentage(cal["cer"]),percentage(hold["cer"]),percentage(hold["wer"]),row["mean_ms"].isDouble()?QString::number(row["mean_ms"].toDouble(),'f',1):QStringLiteral("—"),
            row["p95_ms"].isDouble()?QString::number(row["p95_ms"].toDouble(),'f',1):QStringLiteral("—"),row["pages_per_minute"].isDouble()?QString::number(row["pages_per_minute"].toDouble(),'f',1):QStringLiteral("—"),
            row["peak_rss_bytes"].isDouble()?QString::number(row["peak_rss_bytes"].toDouble()/1048576,'f',1):QStringLiteral("—"),row["complete"].toBool()?tr("Complete"):tr("Incomplete / failed")};
        for(int col=0;col<cells.size();++col)results->setItem(i,col,new QTableWidgetItem(cells[col]));results->item(i,0)->setData(Qt::UserRole,row["pipeline"].toString());
    }
    const QString winner=report["advisory_recommendation"].toString();QString label;
    for(const auto &value:report["pipelines"].toArray())if(value.toObject()["id"].toString()==winner)label=value.toObject()["label"].toString();
    status->setText(report["cancelled"].toBool()?tr("Cancelled. Completed runs remain available for inspection and export."):label.isEmpty()?tr("No complete pipeline can be recommended. Inspect failed runs."):tr("Advisory choice by calibration CER: %1. Check held-out scores before choosing a pipeline for the book. Memory scope is recorded per run; a VL bridge’s RSS excludes its upstream GPU model.").arg(label));
}
void ExperimentDialog::openReport()
{
    if(watcher.isRunning())return;
    const QString path=QFileDialog::getOpenFileName(this,tr("Open experiment report"),{},tr("YAGF experiment (*.yagf-experiment.json *.json)"));
    if(path.isEmpty())return;QFile input(path);
    if(!input.open(QIODevice::ReadOnly) || input.size()>128*1024*1024){status->setText(tr("Could not read a bounded experiment report."));return;}
    const auto data=QJsonDocument::fromJson(input.readAll());const auto object=data.object();
    if(!data.isObject() || object["schema"]!="yagf-experiment/1" || !object["records"].isArray() || object["records"].toArray().size()>256 ||
        !object["pipelines"].isArray() || object["pipelines"].toArray().size()>16 || !object["references"].isArray() || object["references"].toArray().size()>64){status->setText(tr("Incompatible experiment report."));return;}
    report=object;report["summary"]=experiments::summarizeExperiment(report);showResults();save->setEnabled(true);inspect->setEnabled(true);
    status->setText(status->text()+tr(" Loaded report; source SHA-256: %1").arg(report["source_sha256"].toString()));
}

void ExperimentDialog::saveReport()
{
    const QString path=QFileDialog::getSaveFileName(this,tr("Save experiment report"),{},tr("YAGF experiment (*.yagf-experiment.json)"));if(path.isEmpty())return;QSaveFile file(path);
    const auto bytes=QJsonDocument(report).toJson();if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())QMessageBox::warning(this,tr("Experiment report"),file.errorString());
}
void ExperimentDialog::inspectReading()
{
    const int row=results->currentRow();if(row<0)return;const QString id=results->item(row,0)->data(Qt::UserRole).toString();
    QDialog dialog(this);dialog.setWindowTitle(tr("Reference and recorded reading"));dialog.resize(1150,850);
    auto *layout=new QVBoxLayout(&dialog);auto *choice=new QComboBox;auto *details=new QLabel;details->setWordWrap(true);
    auto *split=new QSplitter;auto *truth=new QPlainTextEdit,*reading=new QPlainTextEdit;truth->setReadOnly(true);reading->setReadOnly(true);split->addWidget(truth);split->addWidget(reading);
    layout->addWidget(choice);layout->addWidget(new QLabel(tr("Verified reference (left) · Candidate OCR (right)")));layout->addWidget(details);layout->addWidget(split);
    auto *technical=new QCheckBox(tr("Show technical run details"));auto *json=new QPlainTextEdit;json->setReadOnly(true);json->hide();layout->addWidget(technical);layout->addWidget(json);connect(technical,&QCheckBox::toggled,json,&QPlainTextEdit::setVisible);
    QJsonArray selected;for(const auto &v:report["records"].toArray()){const auto r=v.toObject();if(r["pipeline"].toString()!=id)continue;selected.append(r);choice->addItem(tr("Page %1 · %2 · repetition %3").arg(r["page"].toInt()).arg(r["split"].toString()).arg(r["repetition"].toInt()));}
    const auto referencesJson=report["references"].toArray();
    const auto show=[selected,referencesJson,truth,reading,details,json](int index){
        if(index<0 || index>=selected.size())return;const auto r=selected[index].toObject();
        for(const auto &value:referencesJson)if(value.toObject()["page"]==r["page"])truth->setPlainText(value.toObject()["text"].toString());
        core::OcrDocument hypothesis;QString error;
        if(core::OcrDocument::fromJson(r["hypothesis"].toObject(),&hypothesis,&error))reading->setPlainText(hypothesis.plainText());else reading->setPlainText(r["error"].toString(error));
        const auto scores=r["scores"].toObject();
        details->setText(QStringLiteral("CER %1 · WER %2 · %3 ms · %4 DPI%5").arg(percentage(scores["cer"].toObject()["rate"]),percentage(scores["wer"].toObject()["rate"]))
            .arg(r["recognition_ms"].toDouble(),0,'f',1).arg(r["dpi"].toInt()).arg(r.contains("error")?QStringLiteral(" · ")+r["error"].toString():QString()));
        json->setPlainText(QString::fromUtf8(QJsonDocument(r).toJson()));
    };connect(choice,&QComboBox::currentIndexChanged,&dialog,show);show(0);dialog.exec();
}
}
