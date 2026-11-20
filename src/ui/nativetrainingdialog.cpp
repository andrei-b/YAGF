#include "nativetrainingdialog.h"
#include "aipageicons.h"
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
namespace yagf::ui {
using namespace yagf::processing;
namespace {
QString cachePath(const QString &source) {
    const auto path=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/native-training"; QDir().mkpath(path);
    return QDir(path).filePath(source+".yagf-training.json");
}
QJsonObject datasetJson(const NativeTrainingDataset &dataset) {
    QJsonArray samples; for (const auto &sample:dataset.samples) samples.append(trainingSampleJson(sample));
    return {{"protocol","yagf-native-training/1"},{"source_id",dataset.sourceId},{"samples",samples}};
}
bool modelPath(const QString &path) {return QRegularExpression("^/models/[a-f0-9-]{36}$").match(path).hasMatch();}
}
NativeTrainingDialog::NativeTrainingDialog(core::OcrDocument document,int currentPage,QWidget *parent):QDialog(parent),book(std::move(document))
{
    setObjectName("nativeTrainingDialog"); setWindowTitle(tr("OCR training and adaptation")); setWindowIcon(aiPageAreasIcon()); resize(1120,800);
    auto *layout=new QVBoxLayout(this);
    auto *intro=new QLabel(tr("Review line images and their exact printed text. Train an engine-native model, compare it with the original on separate validation examples, then choose whether to use it. Existing symbol rules continue to work alongside it."),this); intro->setWordWrap(true); layout->addWidget(intro);
    QSettings settings;
    auto *host=new QHBoxLayout; host->addWidget(new QLabel(tr("Training host:"),this));
    endpoint=new QLineEdit(settings.value("nativeTraining/url","http://127.0.0.1:8772").toString(),this); endpoint->setObjectName("nativeTrainingEndpoint"); host->addWidget(endpoint,1);
    token=new QLineEdit(this); token->setEchoMode(QLineEdit::Password); token->setPlaceholderText(tr("Optional session token")); host->addWidget(token);
    connectButton=new QPushButton(tr("Connect"),this); host->addWidget(connectButton); layout->addLayout(host);
    hostStatus=new QLabel(tr("Checking the training host…"),this); hostStatus->setTextFormat(Qt::PlainText); hostStatus->setWordWrap(true); layout->addWidget(hostStatus);
    auto *options=new QHBoxLayout; profiles=new QComboBox(this); profiles->setObjectName("nativeTrainingProfile"); profiles->setMinimumWidth(270); options->addWidget(new QLabel(tr("Engine / base model:"),this)); options->addWidget(profiles,1);
    scope=new QComboBox(this); scope->addItem(tr("This book"),"project"); scope->addItem(tr("Reusable across projects"),"shared"); options->addWidget(scope);
    iterations=new QSpinBox(this); iterations->setRange(1,10000); iterations->setValue(100); iterations->setToolTip(tr("Tesseract iterations. Kraken/PaddleOCR use epochs, limited to 100.")); options->addWidget(new QLabel(tr("Training budget:"),this)); options->addWidget(iterations); layout->addLayout(options);
    auto *range=new QHBoxLayout; first=new QSpinBox(this); last=new QSpinBox(this); first->setRange(1,std::max(1,book.totalPages)); last->setRange(1,std::max(1,book.totalPages)); first->setValue(currentPage); last->setValue(currentPage);
    range->addWidget(new QLabel(tr("Source pages:"),this)); range->addWidget(first); range->addWidget(new QLabel(tr("to"),this)); range->addWidget(last);
    collectButton=new QPushButton(tr("Prepare samples"),this); collectButton->setObjectName("prepareNativeSamples"); range->addWidget(collectButton);
    auto *import=new QPushButton(tr("Import dataset…"),this); range->addWidget(import); auto *exportButton=new QPushButton(tr("Export engine training bundle…"),this); range->addWidget(exportButton); range->addStretch(); layout->addLayout(range);
    counts=new QLabel(this); counts->setObjectName("nativeSampleCounts"); counts->setWordWrap(true); layout->addWidget(counts);
    auto *splitter=new QSplitter(this); samples=new QListWidget(splitter); samples->setObjectName("nativeTrainingSamples"); samples->setIconSize({180,58});
    auto *review=new QWidget(splitter); auto *reviewLayout=new QVBoxLayout(review); preview=new QLabel(review); preview->setMinimumSize(450,130); preview->setAlignment(Qt::AlignCenter); preview->setStyleSheet("background: white; border: 1px solid #d7e1ed; border-radius: 6px;"); reviewLayout->addWidget(preview,1);
    transcription=new QLineEdit(review); transcription->setObjectName("nativeSampleText"); transcription->setPlaceholderText(tr("Exact text visible in this line image")); transcription->setMaxLength(4096); reviewLayout->addWidget(transcription);
    partition=new QComboBox(review); partition->setObjectName("nativeSamplePartition"); partition->addItem(tr("Training example"),"train"); partition->addItem(tr("Validation example (never trained)"),"validation"); reviewLayout->addWidget(partition);
    auto *decisions=new QHBoxLayout; approveButton=new QPushButton(tr("Approve this example"),review); approveButton->setObjectName("approveNativeExample"); excludeButton=new QPushButton(tr("Exclude"),review); decisions->addWidget(approveButton); decisions->addWidget(excludeButton); reviewLayout->addLayout(decisions);
    approveReviewed=new QPushButton(tr("Approve already-reviewed lines"),review); approveReviewed->setToolTip(tr("Approve lines whose entire text was already reviewed in the document. Newly aligned paragraph lines still require individual approval.")); reviewLayout->addWidget(approveReviewed);
    comparison=new QPlainTextEdit(review); comparison->setReadOnly(true); comparison->setObjectName("nativeValidationResults"); comparison->setPlaceholderText(tr("Original and trained model validation results appear here.")); comparison->setMaximumHeight(170); reviewLayout->addWidget(comparison); splitter->setSizes({430,640}); layout->addWidget(splitter,1);
    auto *library=new QHBoxLayout; library->addWidget(new QLabel(tr("Validated models:"),this)); models=new QComboBox(this); models->setObjectName("nativeModelLibrary"); models->addItem(tr("Current training candidate"),QJsonObject{}); library->addWidget(models,1);
    auto *refresh=new QPushButton(tr("Refresh models"),this); library->addWidget(refresh); layout->addLayout(library);
    continueFromModel=new QCheckBox(tr("Continue training from the selected validated model when available"),this); continueFromModel->setChecked(true); layout->addWidget(continueFromModel);
    progress=new QProgressBar(this); progress->setRange(0,0); progress->hide(); layout->addWidget(progress);
    message=new QLabel(tr("Prepare and approve examples before training. Nothing changes the active OCR model until you choose Use model."),this); message->setObjectName("nativeTrainingMessage"); message->setTextFormat(Qt::PlainText); message->setWordWrap(true); layout->addWidget(message);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close,this); trainButton=buttons->addButton(tr("Train approved examples"),QDialogButtonBox::ActionRole); trainButton->setObjectName("trainNativeModel"); useButton=buttons->addButton(tr("Use model"),QDialogButtonBox::AcceptRole); useButton->setObjectName("useNativeModel"); stopButton=buttons->addButton(tr("Cancel operation"),QDialogButtonBox::ActionRole); layout->addWidget(buttons);
    pollTimer=new QTimer(this); pollTimer->setInterval(1000); connect(pollTimer,&QTimer::timeout,this,&NativeTrainingDialog::poll);
    connect(connectButton,&QPushButton::clicked,this,&NativeTrainingDialog::connectHost); connect(collectButton,&QPushButton::clicked,this,&NativeTrainingDialog::collect);
    connect(import,&QPushButton::clicked,this,&NativeTrainingDialog::importDataset); connect(exportButton,&QPushButton::clicked,this,&NativeTrainingDialog::exportBundle);
    connect(approveButton,&QPushButton::clicked,this,[this]{decision("approved");}); connect(excludeButton,&QPushButton::clicked,this,[this]{decision("excluded");});
    connect(approveReviewed,&QPushButton::clicked,this,[this]{saveText(); for (auto &sample:dataset.samples) if (sample.previouslyReviewed) sample.decision="approved"; refreshSamples(samples->currentRow());});
    connect(samples,&QListWidget::currentRowChanged,this,&NativeTrainingDialog::showSample); connect(transcription,&QLineEdit::editingFinished,this,&NativeTrainingDialog::saveText);
    connect(transcription,&QLineEdit::textEdited,this,[this]{if (samples->currentRow()>=0) {dataset.samples[samples->currentRow()].decision="pending"; dataset.samples[samples->currentRow()].previouslyReviewed=false; refreshButtons();}});
    connect(partition,&QComboBox::currentIndexChanged,this,[this]{if (showing || samples->currentRow()<0) return; auto &current=dataset.samples[samples->currentRow()]; QSet<int> pages; for (const auto &s:dataset.samples) pages.insert(s.page); for (auto &s:dataset.samples) if (pages.size()==1 ? s.id==current.id : s.page==current.page) s.partition=partition->currentData().toString(); refreshSamples(samples->currentRow());});
    connect(profiles,&QComboBox::currentIndexChanged,this,[this]{
        const bool tess=profiles->currentData().toJsonObject()["engine"]=="tesseract";
        iterations->setRange(1,tess ? 10000 : 100);
        iterations->setValue(tess ? 1000 : 10);
        iterations->setSuffix(tess ? tr(" iterations") : tr(" epochs"));
        refreshButtons();
    }); connect(models,&QComboBox::currentIndexChanged,this,[this]{showModel(models->currentData().toJsonObject()); refreshButtons();});
    connect(refresh,&QPushButton::clicked,this,&NativeTrainingDialog::loadModels); connect(trainButton,&QPushButton::clicked,this,&NativeTrainingDialog::train); connect(useButton,&QPushButton::clicked,this,&NativeTrainingDialog::useModel); connect(stopButton,&QPushButton::clicked,this,&NativeTrainingDialog::stopWork); connect(buttons,&QDialogButtonBox::rejected,this,&NativeTrainingDialog::reject);
    connect(endpoint,&QLineEdit::textChanged,this,[this]{refreshButtons();});
    connect(token,&QLineEdit::textChanged,this,[this]{refreshButtons();});
    connect(&watcher,&QFutureWatcherBase::finished,this,&NativeTrainingDialog::finished); refreshButtons();
}
NativeTrainingDialog::~NativeTrainingDialog() {stop.request_stop(); if (cancelled) cancelled->store(true); watcher.waitForFinished(); saveCache();}
QString NativeTrainingDialog::sessionToken() const {return token->text();}
void NativeTrainingDialog::showEvent(QShowEvent *event) {QDialog::showEvent(event); if (!opened) {opened=true; QTimer::singleShot(0,this,&NativeTrainingDialog::connectHost);}}
void NativeTrainingDialog::reject() {saveText(); saveCache(); if (activeJob) {closing=true; stopWork(); return;} stop.request_stop(); if (cancelled) cancelled->store(true); QDialog::reject();}
void NativeTrainingDialog::run(QString workKind,std::function<NativeTrainingWork()> task) {if (busy) return; kind=workKind; busy=true; stop=std::stop_source{}; cancelled=std::make_shared<std::atomic_bool>(false); refreshButtons(); watcher.setFuture(QtConcurrent::run(std::move(task)));}
void NativeTrainingDialog::request(QString workKind,const QByteArray &method,const QString &path,const QJsonObject &body)
{
    if (busy) return;
    const auto url=QUrl(endpoint->text().trimmed()); const auto secret=token->text();
    // The stop source is initialized here so the captured token belongs to this call.
    kind=workKind; busy=true; stop=std::stop_source{}; const auto stopToken=stop.get_token(); refreshButtons();
    watcher.setFuture(QtConcurrent::run([url,secret,method,path,body,stopToken]{NativeTrainingWork work; auto response=nativeTrainingRequest(url,secret,method,path,body,stopToken); if (!response) work.error=response.error().cancelled ? "Cancelled" : response.error().message; else work.response=*response; return work;}));
}
void NativeTrainingDialog::connectHost() {
    if (busy || activeJob) return;
    connectedEndpoint.clear(); connectedToken.clear(); candidate={}; profiles->clear(); models->clear();
    models->addItem(tr("Current training candidate"),QJsonObject{}); comparison->clear();
    QSettings().setValue("nativeTraining/url",endpoint->text().trimmed()); request("connect","GET","/v1/capabilities");
}
void NativeTrainingDialog::collect()
{
    if (busy || activeJob) return; saveText(); saveCache(); const int start=first->value(),end=last->value();
    if (start>end) {message->setText(tr("Choose a valid page range.")); return;}
    kind="collect"; busy=true; stop=std::stop_source{}; cancelled=std::make_shared<std::atomic_bool>(false); auto cancellation=cancelled; refreshButtons();
    watcher.setFuture(QtConcurrent::run([document=book,start,end,cancellation]{
        NativeTrainingWork work; auto collected=collectNativeTrainingSamples(document,start,end,*cancellation);
        if (!collected) {work.error=collected.error(); return work;}
        work.dataset=std::move(*collected);
        QFile cache(cachePath(work.dataset.sourceId)); if (cache.open(QIODevice::ReadOnly) && cache.size()<=48*1024*1024) {
            const auto root=QJsonDocument::fromJson(cache.readAll()).object(); auto old=readTrainingSamples(root["samples"]);
            if (old && root["source_id"]==work.dataset.sourceId) for (auto &sample:work.dataset.samples) {
                const auto match=std::ranges::find(*old,sample.id,&NativeTrainingSample::id);
                if (match!=old->end()) sample=*match;
            }
        }
        return work;
    }));
}
void NativeTrainingDialog::saveCache()
{
    if (dataset.sourceId.isEmpty() || dataset.samples.isEmpty()) return;
    QSaveFile file(cachePath(dataset.sourceId)); if (file.open(QIODevice::WriteOnly)) {file.write(QJsonDocument(datasetJson(dataset)).toJson(QJsonDocument::Compact)); file.commit();}
}
void NativeTrainingDialog::refreshSamples(int selected)
{
    const QSignalBlocker blocker(samples); samples->clear(); int train=0,validation=0,pending=0;
    for (const auto &s:dataset.samples) {
        QImage image; image.loadFromData(s.png,"PNG"); auto *item=new QListWidgetItem(QIcon(QPixmap::fromImage(image.scaled(180,58,Qt::KeepAspectRatio,Qt::SmoothTransformation))),tr("Page %1 · %2 · %3\n%4").arg(s.page).arg(s.decision,s.partition,s.text.left(100)),samples);
        item->setToolTip(s.text); item->setSizeHint({360,78});
        if (s.decision=="approved") {if (s.partition=="train") ++train; else ++validation;} else if (s.decision=="pending") ++pending;
    }
    counts->setText(tr("%1 training · %2 validation approved · %3 need review · %4 skipped. Validation pages are kept out of training; a single-page set uses a sample split.").arg(train).arg(validation).arg(pending).arg(dataset.skipped));
    samples->setCurrentRow(dataset.samples.isEmpty() ? -1 : std::clamp(selected,0,int(dataset.samples.size())-1)); showSample(samples->currentRow()); refreshButtons();
}
void NativeTrainingDialog::showSample(int index)
{
    saveText(); editingSample=index; showing=true; if (index<0 || index>=dataset.samples.size()) {transcription->clear(); preview->clear(); showing=false; return;}
    const auto &sample=dataset.samples[index]; QImage image; image.loadFromData(sample.png,"PNG"); preview->setPixmap(QPixmap::fromImage(image.scaled(preview->size()-QSize(16,16),Qt::KeepAspectRatio,Qt::SmoothTransformation))); transcription->setText(sample.text); partition->setCurrentIndex(partition->findData(sample.partition)); showing=false; refreshButtons();
}
void NativeTrainingDialog::saveText()
{
    const int index=editingSample; if (showing || index<0 || index>=dataset.samples.size()) return;
    const auto text=transcription->text().trimmed().normalized(QString::NormalizationForm_C); auto &sample=dataset.samples[index];
    if (sample.text!=text) {sample.text=text; sample.decision="pending"; sample.previouslyReviewed=false;}
}
void NativeTrainingDialog::decision(const QString &value)
{
    saveText(); const int index=samples->currentRow(); if (index<0) return;
    const auto text=dataset.samples[index].text;
    if (value=="approved" && (text.isEmpty() || std::ranges::any_of(text.toUcs4(),[](char32_t c){return !QChar::isPrint(c) || (QChar::isSpace(c) && c!=' ');}))) {message->setText(tr("Enter one nonempty printed line before approving.")); return;}
    dataset.samples[index].decision=value; refreshSamples(index+1<dataset.samples.size() ? index+1 : index);
}
void NativeTrainingDialog::refreshButtons()
{
    const bool idle=!busy && !activeJob; int train=0,validation=0;
    const bool connected=!connectedEndpoint.isEmpty() && connectedEndpoint==endpoint->text().trimmed() && connectedToken==token->text();
    for (const auto &s:dataset.samples) if (s.decision=="approved") {if (s.partition=="train") ++train; else ++validation;}
    const auto profile=profiles->currentData().toJsonObject();
    trainButton->setEnabled(idle && connected && profile["ready"].toBool() && train>=2 && validation>=1);
    const auto model=models->currentIndex()==0 ? candidate : models->currentData().toJsonObject();
    useButton->setEnabled(idle && connected && model["status"]=="complete" && model["eligible"].toBool() && modelPath(model["model_url"].toString()));
    connectButton->setEnabled(idle); collectButton->setEnabled(idle); profiles->setEnabled(idle); endpoint->setEnabled(idle); token->setEnabled(idle); iterations->setEnabled(idle); scope->setEnabled(idle); first->setEnabled(idle); last->setEnabled(idle); continueFromModel->setEnabled(idle);
    transcription->setEnabled(idle && samples->currentRow()>=0); partition->setEnabled(idle && samples->currentRow()>=0); approveButton->setEnabled(idle && samples->currentRow()>=0); excludeButton->setEnabled(idle && samples->currentRow()>=0); approveReviewed->setEnabled(idle);
    progress->setVisible(busy || activeJob); stopButton->setVisible(busy || activeJob);
}
void NativeTrainingDialog::train()
{
    saveText(); saveCache(); QJsonArray approved;
    for (const auto &sample:dataset.samples) if (sample.decision=="approved") approved.append(trainingSampleJson(sample));
    const auto profile=profiles->currentData().toJsonObject();
    const auto model=models->currentIndex()==0 ? candidate : models->currentData().toJsonObject();
    QJsonObject body{{"protocol","yagf-native-training/1"},{"source_id",dataset.sourceId},{"profile_id",profile["id"]},
        {"scope",scope->currentData().toString()},{"iterations",iterations->value()},{"samples",approved}};
    if (continueFromModel->isChecked() && model["eligible"].toBool()) {
        if (model["profile_id"]!=profile["id"]) {message->setText(tr("Choose the selected model's engine profile, or uncheck Continue training to start from the stock model.")); return;}
        body.insert("base_model_id",model["id"]);
    }
    request("train","POST","/v1/jobs",body);
}
void NativeTrainingDialog::stopWork()
{
    if (busy && kind=="train") {cancelAfterSubmit=true; return;}
    if (busy) {if (activeJob) cancelAfterSubmit=true; stop.request_stop(); if (cancelled) cancelled->store(true); return;}
    if (activeJob) request("cancel","POST","/v1/jobs/"+jobId+"/cancel",{{"protocol","yagf-native-training/1"}});
}
void NativeTrainingDialog::poll() {if (!busy && activeJob) request("poll","GET","/v1/jobs/"+jobId);}
void NativeTrainingDialog::loadModels() {if (!busy && !activeJob) request("models","GET","/v1/models");}
void NativeTrainingDialog::showModel(const QJsonObject &model)
{
    const auto shown=model.isEmpty() ? candidate : model;
    if (!shown.contains("baseline")) return;
    const auto before=shown["baseline"].toObject(),after=shown["candidate"].toObject();
    QString text=tr("Original validation CER: %1%\nTrained validation CER: %2%\n%3\n")
        .arg(before["cer"].toDouble()*100,0,'f',2).arg(after["cer"].toDouble()*100,0,'f',2)
        .arg(shown["eligible"].toBool() ? tr("Candidate is no worse on the held-out examples. Use model enables it explicitly.") : tr("Candidate worsened validation. The original model remains active."));
    for (const auto &entry:after["predictions"].toArray()) {const auto p=entry.toObject(); text+=tr("\nExpected: %1\nRecognized: %2\n").arg(p["truth"].toString(),p["prediction"].toString());}
    comparison->setPlainText(text);
}
void NativeTrainingDialog::useModel()
{
    const auto model=models->currentIndex()==0 ? candidate : models->currentData().toJsonObject();
    const QString path=model["model_url"].toString(); if (!model["eligible"].toBool() || !modelPath(path)) return;
    selectedUrl=QUrl(endpoint->text().trimmed()); selectedUrl.setPath(path); saveCache(); accept();
}
void NativeTrainingDialog::finished()
{
    busy=false; auto work=watcher.result(); const QString completed=kind; kind.clear();
    if (!work.error.isEmpty()) {message->setText(work.error); if (completed=="connect") hostStatus->setText(work.error);}
    else if (completed=="connect") {
        connectedEndpoint=endpoint->text().trimmed(); connectedToken=token->text();
        profiles->clear(); QStringList states;
        for (const auto &value:work.response["profiles"].toArray()) {const auto profile=value.toObject(); profiles->addItem(profile["label"].toString()+(profile["ready"].toBool() ? QString() : tr(" (not ready)")),profile); profiles->setItemData(profiles->count()-1,profile["reason"].toString(),Qt::ToolTipRole); states.append(profile["label"].toString()+": "+(profile["ready"].toBool() ? tr("ready") : profile["reason"].toString()));}
        hostStatus->setText(states.join(" · "));
    } else if (completed=="collect") {
        editingSample=-1; dataset=std::move(work.dataset); refreshSamples(); saveCache(); message->setText(tr("Review each image against its transcription. Editing text returns that example to pending review."));
    } else if (completed=="train") {
        jobId=work.response["id"].toString();
        activeJob=QRegularExpression("^[a-f0-9-]{36}$").match(jobId).hasMatch();
        if (!activeJob) message->setText(tr("Training host returned an invalid job ID.")); else {message->setText(tr("Native training started. The current OCR model remains active.")); pollTimer->start();}
    } else if (completed=="poll" || completed=="cancel") {
        candidate=work.response; message->setText(candidate["message"].toString());
        const auto status=candidate["status"].toString(); activeJob=status=="running" || status=="queued";
        if (!activeJob) {pollTimer->stop(); if (status=="complete") {models->setCurrentIndex(0); showModel(candidate);}}
    } else if (completed=="models") {
        models->clear(); models->addItem(tr("Current training candidate"),QJsonObject{});
        for (const auto &value:work.response["models"].toArray()) {
            const auto model=value.toObject();
            if (model["scope"]=="shared" || model["source_id"]==dataset.sourceId) {
                models->addItem(model["label"].toString()+" · "+model["id"].toString().left(8)+tr(" · CER %1%").arg(model["candidate"].toObject()["cer"].toDouble()*100,0,'f',2),model);
                if (QUrl(book.nativeOcrUrl).path()==model["model_url"].toString()) models->setCurrentIndex(models->count()-1);
            }
        }
    }
    refreshButtons();
    if (cancelAfterSubmit && activeJob) {cancelAfterSubmit=false; stopWork(); return;}
    if (!activeJob) cancelAfterSubmit=false;
    if (closing) {if (activeJob && completed!="cancel") stopWork(); else {activeJob=false; QDialog::reject();} return;}
    if (completed=="connect" && initialCollect) {initialCollect=false; collect();}
    else if (completed=="collect" && work.error.isEmpty()) loadModels();
}
void NativeTrainingDialog::exportBundle()
{
    if (busy || activeJob) return; saveText();
    const auto path=QFileDialog::getExistingDirectory(this,tr("Choose an empty training bundle folder")); if (path.isEmpty()) return;
    const auto snapshot=dataset; run("export",[path,snapshot]{NativeTrainingWork work; const std::atomic_bool stop=false; auto result=writeNativeTrainingBundle(path,snapshot,stop); if (!result) work.error=result.error(); return work;});
}
void NativeTrainingDialog::importDataset()
{
    if (busy || activeJob) return; const auto path=QFileDialog::getOpenFileName(this,tr("Import reviewed training examples"),{},tr("YAGF training dataset (*.json)")); if (path.isEmpty()) return;
    QFile file(path); if (!file.open(QIODevice::ReadOnly) || file.size()>48*1024*1024) {message->setText(tr("Could not read a bounded training dataset.")); return;}
    const auto json=QJsonDocument::fromJson(file.readAll()).object(); auto imported=readTrainingSamples(json["samples"]);
    const auto source=json["source_id"].toString();
    if (!imported || json["protocol"]!="yagf-native-training/1" || !QRegularExpression("^[a-f0-9]{64}$").match(source).hasMatch()) {message->setText(imported ? tr("Invalid training dataset.") : imported.error()); return;}
    editingSample=-1; dataset={source,*imported,0}; refreshSamples(); message->setText(tr("Imported examples retain their review decisions. Check the engine and validation split before training."));
}
}
