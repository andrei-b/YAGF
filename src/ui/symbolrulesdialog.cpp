#include "symbolrulesdialog.h"
#include "symbolaugmentation.h"
#include "exportpage.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFile>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
#include <random>

namespace yagf::ui {
using namespace yagf::processing;
namespace {
QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString verdict(const QString &v) { return v == "positive" ? QObject::tr("Match") : v == "negative" ? QObject::tr("Non-match") : QObject::tr("Needs review"); }
SymbolRuleWork remoteTrain(const core::SymbolRule &rule, const QUrl &base, const QString &token, std::stop_token stop)
{
    SymbolRuleWork result;
    const auto local = core::trainSymbolRule(rule);
    if (!local) { result.error = local.error(); return result; }
    QJsonArray samples;
    for (const auto &e : rule.examples) if (e.decision != "pending") samples.append(QJsonObject{
        {"mask", QString::fromLatin1(e.mask.toBase64())}, {"decision", e.decision}});
    QUrl url(base); url.setPath(base.path().trimmed().remove(QRegularExpression("/$"))+"/v1/train");
    QNetworkAccessManager network;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer "+token.toUtf8());
    auto *reply = network.post(request, QJsonDocument(QJsonObject{{"protocol", "yagf-symbol-training/1"},
        {"training_hash", QString::fromLatin1(core::symbolTrainingHash(rule))}, {"examples", samples}}).toJson(QJsonDocument::Compact));
    QEventLoop loop; QTimer timer; QElapsedTimer elapsed; elapsed.start();
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] { if (reply->bytesAvailable() > 128*1024) reply->abort(); });
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] { if (stop.stop_requested() || elapsed.elapsed() > 60000) reply->abort(); });
    timer.start(50); loop.exec();
    if (stop.stop_requested()) { result.cancelled = true; return result; }
    if (reply->error() != QNetworkReply::NoError) {
        result.error = QObject::tr("Training host: %1").arg(reply->errorString()); return result;
    }
    const auto response = QJsonDocument::fromJson(reply->readAll()).object();
    if (response["protocol"] != "yagf-symbol-training/1" || response["model"].toObject() != *local) {
        result.error = QObject::tr("The training host returned an incompatible or inconsistent model."); return result;
    }
    result.model = response["model"].toObject(); return result;
}
}

class SymbolCropView final : public QWidget {
public:
    QImage image;
    QRect selected, context;
    explicit SymbolCropView(QWidget *parent) : QWidget(parent) { setMinimumSize(360, 180); setMouseTracking(true); }
    void setImage(QImage page, QRect bounds)
    {
        image = std::move(page); selected = bounds.intersected(image.rect());
        const int margin = std::max(24, selected.height());
        context = selected.adjusted(-margin, -margin, margin, margin).intersected(image.rect()); update();
    }
protected:
    QRectF target() const
    {
        if (context.isEmpty()) return {};
        const QSizeF fitted = QSizeF(context.size()).scaled(size()-QSize(12,12), Qt::KeepAspectRatio);
        return {QPointF((width()-fitted.width())/2, (height()-fitted.height())/2), fitted};
    }
    QPoint source(QPointF p) const
    {
        const auto t = target(); if (t.isEmpty()) return {};
        return QPoint(qRound(context.x()+(p.x()-t.x())*context.width()/t.width()),
                      qRound(context.y()+(p.y()-t.y())*context.height()/t.height()));
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this); p.fillRect(rect(), QColor("#e9eef5"));
        if (image.isNull() || context.isEmpty()) return;
        const auto t = target(); p.drawImage(t, image, context);
        const QRectF box(t.x()+(selected.x()-context.x())*t.width()/context.width(),
            t.y()+(selected.y()-context.y())*t.height()/context.height(),
            selected.width()*t.width()/context.width(), selected.height()*t.height()/context.height());
        p.setPen(QPen(QColor("#216ec1"), 2)); p.setBrush(QColor(33,110,193,35)); p.drawRect(box);
    }
    void mousePressEvent(QMouseEvent *e) override { if (e->button() == Qt::LeftButton) { anchor = source(e->position()); dragging = true; } }
    void mouseMoveEvent(QMouseEvent *e) override { if (dragging) { selected = QRect(anchor, source(e->position())).normalized().intersected(context); update(); } }
    void mouseReleaseEvent(QMouseEvent *e) override { mouseMoveEvent(e); dragging = false; }
private:
    QPoint anchor; bool dragging = false;
};

SymbolRulesDialog::SymbolRulesDialog(core::OcrDocument book, QVector<core::SymbolRule> shared, QWidget *parent,
    QImage seed, QRect seedBounds, int seedPage, QString selected, int wordIndex, int characterStart, int characterLength)
    : QDialog(parent), book(std::move(book))
{
    QFile source(this->book.sourcePath);
    if (source.open(QIODevice::ReadOnly)) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (hash.addData(&source)) sourceIdentity=QString::fromLatin1(hash.result().toHex());
    }
    setObjectName("symbolRulesDialog"); setWindowTitle(tr("Symbol recognition rules")); resize(1020, 800);
    for (const auto &r : this->book.symbolRules) entries.append({r, false});
    for (const auto &r : shared) entries.append({r, true});
    auto *root = new QVBoxLayout(this);
    auto *help = new QLabel(tr("Teach OCR a printed symbol such as ψ or ☺. Find examples, review matches and lookalikes, then train and apply the recognizer. Rules can belong to this book or be shared across projects."));
    help->setWordWrap(true); root->addWidget(help);
    panel = new QWidget(this); root->addWidget(panel, 1); auto *layout = new QVBoxLayout(panel);
    auto *ruleRow = new QHBoxLayout; rules = new QComboBox; rules->setObjectName("symbolRuleList"); ruleRow->addWidget(rules, 1);
    for (const auto &[title, action] : QVector<QPair<QString, std::function<void()>>>{
        {tr("Import…"), [this] { importRules(); }}, {tr("Export…"), [this] { exportRules(); }},
        {tr("Delete rule"), [this] { if (rules->currentIndex() >= 0) { entries.removeAt(rules->currentIndex()); appliedPages.clear(); refreshRules(); } }}}) {
        auto *b = new QPushButton(title); ruleRow->addWidget(b); connect(b, &QPushButton::clicked, this, action);
    }
    layout->addLayout(ruleRow);
    auto *form = new QFormLayout; label = new QLineEdit; label->setObjectName("symbolRuleLabel");
    language = new QLineEdit; language->setObjectName("symbolRuleLanguage"); language->setPlaceholderText(tr("rus, eng, rus+eng; empty for all languages"));
    scope = new QComboBox; scope->setObjectName("symbolRuleScope"); scope->addItems({tr("This book"), tr("All projects")});
    enabled = new QCheckBox(tr("Use this rule during OCR")); enabled->setObjectName("symbolRuleEnabled");
    form->addRow(tr("Recognize as:"), label); form->addRow(tr("OCR languages:"), language); form->addRow(tr("Apply rule to:"), scope); form->addRow(enabled);
    layout->addLayout(form);
    state = new QLabel; state->setWordWrap(true); state->setObjectName("symbolModelState"); layout->addWidget(state);
    auto *split = new QSplitter; layout->addWidget(split, 1);
    examples = new QListWidget; examples->setObjectName("symbolTrainingExamples"); examples->setIconSize({72,72}); examples->setSelectionMode(QAbstractItemView::ExtendedSelection); split->addWidget(examples);
    auto *detail = new QWidget; auto *detailLayout = new QVBoxLayout(detail); split->addWidget(detail);
    preview = new QLabel(tr("Select an example to inspect its image.")); preview->setWordWrap(true); detailLayout->addWidget(preview);
    crop = new SymbolCropView(detail); crop->setObjectName("symbolCropView"); detailLayout->addWidget(crop, 1);
    auto *cropHelp = new QLabel(tr("Draw a tight rectangle around the printed symbol. Use Refine crop to load the example's page, then save the crop.")); cropHelp->setWordWrap(true); detailLayout->addWidget(cropHelp);
    auto *cropRow = new QHBoxLayout;
    auto *refineButton = new QPushButton(tr("Refine crop…")); auto *commit = new QPushButton(tr("Save crop / add seed")); commit->setObjectName("saveSymbolCrop");
    cropRow->addWidget(refineButton); cropRow->addWidget(commit); detailLayout->addLayout(cropRow);
    connect(refineButton, &QPushButton::clicked, this, &SymbolRulesDialog::refine); connect(commit, &QPushButton::clicked, this, &SymbolRulesDialog::commitCrop);
    auto *decisions = new QHBoxLayout;
    for (const auto &[title, decision] : QVector<QPair<QString,QString>>{{tr("Approve match"),"positive"}, {tr("Approve non-match"),"negative"}, {tr("Review later"),"pending"}}) {
        auto *b = new QPushButton(title); b->setObjectName("symbolDecision-"+decision); decisions->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, decision] { setDecision(decision); });
    }
    auto *remove = new QPushButton(tr("Remove example")); decisions->addWidget(remove);
    connect(remove, &QPushButton::clicked, this, [this] {
        if (auto *r = currentRule()) { auto selected = examples->selectedItems(); QVector<int> rows;
            for (auto *item : selected) rows.append(examples->row(item)); std::ranges::sort(rows, std::greater<>());
            for (int row : rows) r->examples.removeAt(row); appliedPages.clear(); refreshExamples(); }
    }); layout->addLayout(decisions);
    auto *search = new QHBoxLayout; first = new QSpinBox; last = new QSpinBox;
    first->setRange(1, std::max(1,this->book.totalPages)); last->setRange(1,std::max(1,this->book.totalPages)); last->setValue(last->maximum());
    first->setObjectName("symbolSearchFirstPage"); last->setObjectName("symbolSearchLastPage");
    search->addWidget(new QLabel(tr("Pages:"))); search->addWidget(first); search->addWidget(new QLabel(tr("to"))); search->addWidget(last);
    auto *find = new QPushButton(tr("Find similar images")); find->setObjectName("findSymbolExamples"); search->addWidget(find); layout->addLayout(search);
    connect(find, &QPushButton::clicked, this, &SymbolRulesDialog::mine);
    auto *training = new QHBoxLayout; backend = new QComboBox; backend->setObjectName("symbolTrainingBackend"); backend->addItems({tr("This computer"), tr("Jetson / remote host")});
    QSettings settings("YAGF", "YAGF Revival"); backend->setCurrentIndex(settings.value("symbols/remoteTraining",false).toBool() ? 1 : 0);
    endpoint = new QLineEdit(settings.value("symbols/trainingUrl", "http://127.0.0.1:8770").toString()); endpoint->setObjectName("symbolTrainingUrl");
    token = new QLineEdit; token->setEchoMode(QLineEdit::Password); token->setPlaceholderText(tr("Optional service token (this session)"));
    auto *trainButton = new QPushButton(tr("Train approved examples")); trainButton->setObjectName("trainSymbolRule");
    training->addWidget(backend); training->addWidget(endpoint, 1); training->addWidget(token); training->addWidget(trainButton); layout->addLayout(training);
    connect(trainButton, &QPushButton::clicked, this, &SymbolRulesDialog::train);
    auto *applyButton = new QPushButton(tr("Preview trained rules on this book…")); applyButton->setObjectName("applySymbolRules"); layout->addWidget(applyButton);
    connect(applyButton, &QPushButton::clicked, this, &SymbolRulesDialog::apply);
    message = new QLabel; message->setWordWrap(true); message->setObjectName("symbolRuleMessage"); root->addWidget(message);
    auto *progressRow = new QHBoxLayout; bar = new QProgressBar; bar->hide(); cancel = new QPushButton(tr("Cancel operation")); cancel->hide();
    cancel->setObjectName("cancelSymbolOperation");
    progressRow->addWidget(bar, 1); progressRow->addWidget(cancel); root->addLayout(progressRow);
    buttons = new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel); root->addWidget(buttons);
    connect(cancel, &QPushButton::clicked, this, [this] { stop.request_stop(); if (cancelled) cancelled->store(true); });
    connect(buttons, &QDialogButtonBox::accepted, this, &SymbolRulesDialog::save); connect(buttons, &QDialogButtonBox::rejected, this, &SymbolRulesDialog::reject);
    connect(&watcher, &QFutureWatcher<SymbolRuleWork>::finished, this, &SymbolRulesDialog::finished);
    connect(rules, &QComboBox::currentIndexChanged, this, &SymbolRulesDialog::showRule);
    connect(examples, &QListWidget::currentRowChanged, this, &SymbolRulesDialog::showExample);
    const auto changed = [this] {
        if (showing || !currentRule()) return;
        auto &e = entries[rules->currentIndex()]; e.rule.label = label->text(); e.rule.language = language->text().trimmed();
        e.rule.enabled = enabled->isChecked(); e.shared = scope->currentIndex() == 1;
        rules->setItemText(rules->currentIndex(),e.rule.label+" · "+scope->currentText());
        state->setText(core::symbolRuleTrained(e.rule) ? tr("Trained. Approved examples are ready for recognition.") : tr("Draft or changed examples. Train before using this rule."));
        appliedPages.clear();
    };
    connect(label, &QLineEdit::textChanged, this, changed); connect(language, &QLineEdit::textChanged, this, changed);
    connect(enabled, &QCheckBox::toggled, this, changed); connect(scope, &QComboBox::currentIndexChanged, this, changed);
    if (!seed.isNull() && !seedBounds.isEmpty()) {
        core::SymbolRule r; r.id = id(); r.label = selected; r.seedPage = seedPage; r.seedBounds = seedBounds;
        const auto *page = this->book.page(seedPage); r.seedDpi = page ? page->renderDpi : 150; r.language = page ? page->language : this->book.language;
        r.seedPng = symbolCropPng(seed.copy(seedBounds), 512); entries.append({r, false});
        refreshRules(); rules->setCurrentIndex(entries.size()-1);
        crop->setImage(seed, seedBounds); seedCrop = true; editingExample = -1;
        message->setText(tr("Confirm the printed symbol's crop and enter its correct reading, then Save crop / add seed. Suggested positions inside a word must be checked."));
        if (page && wordIndex >= 0 && wordIndex < page->words.size() && characterStart >= 0) {
            const auto word = page->words[wordIndex];
            run("seed", [seed, word, characterStart, characterLength, language=r.language](auto stop, const auto &) {
                SymbolRuleWork result; result.image = seed; result.crop = word.bounds;
                const auto cuts = symbolCuts(seed, word, language, stop);
                QRect bounds; int covered = 0;
                for (const auto &cut : cuts) if (cut.start >= characterStart && cut.start+cut.length <= characterStart+characterLength) { bounds |= cut.bounds; covered += cut.length; }
                if (covered == characterLength && !bounds.isEmpty()) result.crop = bounds;
                return result;
            });
        }
    } else refreshRules();
}

SymbolRulesDialog::~SymbolRulesDialog() { stop.request_stop(); if (cancelled) cancelled->store(true); watcher.waitForFinished(); }
core::SymbolRule *SymbolRulesDialog::currentRule() { const int i = rules->currentIndex(); return i >= 0 && i < entries.size() ? &entries[i].rule : nullptr; }
QVector<core::SymbolRule> SymbolRulesDialog::bookRules() const { QVector<core::SymbolRule> out; for (auto e : entries) if (!e.shared) out.append(e.rule); return out; }
QVector<core::SymbolRule> SymbolRulesDialog::sharedRules() const { QVector<core::SymbolRule> out; for (auto e : entries) if (e.shared) out.append(e.rule); return out; }
void SymbolRulesDialog::refreshRules()
{
    const int old = rules->currentIndex();
    { const QSignalBlocker blocker(rules); rules->clear(); for (const auto &e : entries) rules->addItem(e.rule.label+" · "+(e.shared ? tr("All projects") : tr("This book"))); rules->setCurrentIndex(entries.isEmpty() ? -1 : std::clamp(old, 0, int(entries.size()-1))); }
    showRule(rules->currentIndex());
}
void SymbolRulesDialog::showRule(int)
{
    showing = true; seedCrop = false; editingExample = -1;
    auto *r = currentRule(); label->setText(r ? r->label : QString()); language->setText(r ? r->language : QString());
    enabled->setChecked(r && r->enabled); scope->setCurrentIndex(r && entries[rules->currentIndex()].shared ? 1 : 0);
    showing = false; refreshExamples();
}
void SymbolRulesDialog::refreshExamples()
{
    const QSignalBlocker blocker(examples); examples->clear();
    auto *r = currentRule(); int positive = 0, negative = 0, pending = 0;
    if (r) for (const auto &e : r->examples) {
        auto *item = new QListWidgetItem(QIcon(QPixmap::fromImage(QImage::fromData(e.png))),
            tr("Page %1 · %2\nOCR: %3").arg(e.page).arg(verdict(e.decision), e.reading.left(60)), examples);
        item->setToolTip(tr("Box: %1, %2, %3 × %4").arg(e.bounds.x()).arg(e.bounds.y()).arg(e.bounds.width()).arg(e.bounds.height()));
        positive += e.decision == "positive"; negative += e.decision == "negative"; pending += e.decision == "pending";
    }
    state->setText(tr("%1 matches · %2 non-matches · %3 awaiting review. %4").arg(positive).arg(negative).arg(pending).arg(
        r && core::symbolRuleTrained(*r) ? tr("Trained and ready.") : tr("Approve at least two matches and one non-match, then train.")));
}
void SymbolRulesDialog::showExample(int index)
{
    auto *r = currentRule(); if (!r || index < 0 || index >= r->examples.size()) return;
    const auto &e = r->examples[index]; const auto image = QImage::fromData(e.png);
    crop->setImage(image, image.rect()); seedCrop = false; editingExample = -1;
    preview->setText(tr("Page %1 · %2 · OCR: %3").arg(e.page).arg(verdict(e.decision), e.reading));
}
void SymbolRulesDialog::setDecision(const QString &decision)
{
    auto *r = currentRule(); if (!r) return;
    for (auto *item : examples->selectedItems()) r->examples[examples->row(item)].decision = decision;
    appliedPages.clear(); refreshExamples();
}
void SymbolRulesDialog::commitCrop()
{
    auto *r = currentRule(); if (!r || crop->image.isNull() || crop->selected.isEmpty()) return;
    if (!seedCrop && editingExample < 0) { message->setText(tr("Choose Refine crop before changing this saved example.")); return; }
    const auto pixels = crop->image.copy(crop->selected); const auto feature = symbolFeature(pixels);
    if (feature.isEmpty()) { message->setText(tr("Choose a crop containing visible ink.")); return; }
    if (seedCrop) {
        if (r->examples.size() >= core::maximumSymbolExamples) { message->setText(tr("This rule already has 256 examples.")); return; }
        r->seedPng = symbolCropPng(pixels,512); r->seedBounds = crop->selected;
        core::SymbolExample e; e.id=id(); e.page=r->seedPage; e.bounds=r->seedBounds; e.png=symbolCropPng(pixels); e.mask=feature;
        e.source=sourceIdentity; e.reading=label->text(); e.decision="positive"; r->examples.append(e);
        seedCrop=false;
    } else if (editingExample < r->examples.size()) {
        auto &e=r->examples[editingExample];
        if (editingExample==0 && e.bounds==r->seedBounds && e.page==r->seedPage) {
            r->seedPng=symbolCropPng(pixels,512); r->seedBounds=crop->selected;
        }
        e.bounds=crop->selected; e.png=symbolCropPng(pixels); e.mask=feature; e.decision="pending";
    }
    editingExample=-1; appliedPages.clear(); refreshExamples(); message->setText(tr("Crop saved. Review matching and non-matching examples before training."));
}
void SymbolRulesDialog::run(QString operation, std::function<SymbolRuleWork(std::stop_token,const std::atomic_bool &)> task)
{
    if (watcher.isRunning()) return;
    kind=std::move(operation); stop=std::stop_source{}; cancelled=std::make_shared<std::atomic_bool>(false);
    panel->setEnabled(false); buttons->button(QDialogButtonBox::Save)->setEnabled(false); bar->setRange(0,0); bar->show(); cancel->show();
    watcher.setFuture(QtConcurrent::run([task=std::move(task), token=stop.get_token(), cancellation=cancelled] { return task(token,*cancellation); }));
}
void SymbolRulesDialog::progress(int page, int count)
{
    QMetaObject::invokeMethod(this, [this,page,count] { bar->setRange(0,count); bar->setValue(page); message->setText(tr("Processed %1 of %2 pages…").arg(page).arg(count)); }, Qt::QueuedConnection);
}
void SymbolRulesDialog::mine()
{
    auto *r=currentRule(); if (!r || r->seedPng.isEmpty()) return;
    if (r->examples.size() >= core::maximumSymbolExamples) { message->setText(tr("Remove some examples before searching again; this rule already has 256.")); return; }
    if (first->value()>last->value()) { message->setText(tr("Choose an ordered page range.")); return; }
    const auto rule=*r; const auto snapshot=book; const auto source=sourceIdentity; const int start=first->value(), end=last->value();
    run("mine", [this,rule,snapshot,source,start,end](auto stop,const auto &cancelled) {
        SymbolRuleWork result; QTemporaryDir temporary;
        const int capacity=core::maximumSymbolExamples-rule.examples.size();
        std::mt19937 sampling(0x59414746);
        if (!temporary.isValid()) { result.error=tr("Could not create search temporary files."); return result; }
        for (int number=start;number<=end;++number) {
            if (stop.stop_requested() || cancelled.load()) { result.cancelled=true; return result; }
            auto image=readExportPage(snapshot,number,temporary,cancelled);
            if (!image) { result.error=image.error().error; return result; }
            const auto *page=snapshot.page(number); const int dpi=page ? page->renderDpi : qRound(image->dotsPerMeterY()*.0254);
            for (const auto &candidate : findSymbolCandidates(*image,rule,dpi,stop)) {
                const auto duplicate=[&](const auto &e) { return e.source==source && e.page==number &&
                    candidate.bounds.intersected(e.bounds).width()*candidate.bounds.intersected(e.bounds).height() > .6*std::min(candidate.bounds.width()*candidate.bounds.height(),e.bounds.width()*e.bounds.height()); };
                if (std::ranges::any_of(rule.examples,duplicate) || std::ranges::any_of(result.examples,duplicate)) continue;
                core::SymbolExample e; e.id=id(); e.page=number; e.bounds=candidate.bounds; e.mask=candidate.mask;
                e.png=symbolCropPng(image->copy(candidate.bounds)); e.source=source;
                if (page) for (const auto &word : page->words) if (word.bounds.contains(candidate.bounds.center())) { e.reading=word.text.left(256); break; }
                // Reservoir sampling covers the entire requested range, while
                // bounding saved crops instead of stopping on the first pages.
                ++result.proposed;
                if (result.examples.size()<capacity) result.examples.append(e);
                else {
                    const int position=std::uniform_int_distribution<int>(0,result.proposed-1)(sampling);
                    if (position<capacity) result.examples[position]=std::move(e);
                }
            }
            ++result.scanned; progress(number-start+1,end-start+1);
        }
        return result;
    });
}
void SymbolRulesDialog::train()
{
    auto *r=currentRule(); if (!r) return;
    const auto trained=core::trainSymbolRule(*r); if (!trained) { message->setText(trained.error()); return; }
    if (r->label.trimmed().isEmpty()) { message->setText(tr("Enter the correct symbol reading.")); return; }
    const bool remote=backend->currentIndex()==1; const QUrl address(endpoint->text());
    if (remote && ((address.scheme()!="http" && address.scheme()!="https") || address.host().isEmpty() || !address.userInfo().isEmpty() || address.hasQuery() || address.hasFragment())) {
        message->setText(tr("Enter the training service base URL.")); return;
    }
    QSettings settings("YAGF","YAGF Revival"); settings.setValue("symbols/remoteTraining",remote); settings.setValue("symbols/trainingUrl",endpoint->text());
    const auto rule=*r; const QString credential=token->text();
    run("train", [rule,remote,address,credential,trained](auto stop,const auto &) {
        if (remote) return remoteTrain(rule,address,credential,stop);
        SymbolRuleWork result; result.model=*trained; return result;
    });
}
void SymbolRulesDialog::refine()
{
    auto *r=currentRule(); const int index=examples->currentRow(); if (!r || index<0 || index>=r->examples.size()) return;
    const auto e=r->examples[index];
    if (e.source!=sourceIdentity && e.source!=book.sourcePath) { message->setText(tr("This example belongs to another book. Open that book to refine its source crop; the saved image can still be reviewed here.")); return; }
    editingExample=index; const auto snapshot=book;
    run("refine", [snapshot,e](auto,const auto &cancelled) {
        SymbolRuleWork result; QTemporaryDir temporary; auto image=readExportPage(snapshot,e.page,temporary,cancelled);
        if (!image) result.error=image.error().error; else { result.image=*image; result.crop=e.bounds; } return result;
    });
}
void SymbolRulesDialog::apply()
{
    auto all=bookRules(); all+=sharedRules();
    if (!std::ranges::any_of(all,[](const auto &r) { return r.enabled && core::symbolRuleTrained(r); })) { message->setText(tr("Train and enable at least one rule first.")); return; }
    const auto snapshot=book;
    run("apply",[this,snapshot,all](auto stop,const auto &cancelled) {
        SymbolRuleWork result; QTemporaryDir temporary; int done=0;
        for (auto page:snapshot.pages) {
            if (stop.stop_requested() || cancelled.load()) { result.cancelled=true; return result; }
            auto image=readExportPage(snapshot,page.number,temporary,cancelled);
            if (!image) { result.error=image.error().error; return result; }
            const int changes=applySymbolRules(page,*image,all,stop); result.changes+=changes;
            if (changes) result.pages.append(std::move(page)); progress(++done,snapshot.pages.size());
        }
        return result;
    });
}
void SymbolRulesDialog::finished()
{
    const auto result=watcher.result(); panel->setEnabled(true); buttons->button(QDialogButtonBox::Save)->setEnabled(true); bar->hide(); cancel->hide();
    if (closing) { QDialog::reject(); return; }
    if (result.cancelled || stop.stop_requested()) { message->setText(tr("Cancelled. No partial search, training, or application was accepted.")); return; }
    if (!result.error.isEmpty()) { message->setText(result.error); return; }
    if (kind=="seed" || kind=="refine") {
        crop->setImage(result.image,result.crop); seedCrop=kind=="seed";
    } else if (kind=="mine") {
        if (auto *r=currentRule()) r->examples+=result.examples; refreshExamples();
        message->setText(tr("Collected %1 of %2 candidates across %3 pages. Every saved candidate awaits review. When the 256-example limit is reached, examples are sampled across the full page range.").arg(result.examples.size()).arg(result.proposed).arg(result.scanned));
    } else if (kind=="train") {
        if (auto *r=currentRule()) r->model=result.model; appliedPages.clear(); refreshExamples();
        message->setText(tr("Training complete. This symbol recognizer uses only your approved crops. Preview it on the book or save it for future OCR."));
    } else if (kind=="apply") {
        QStringList changes;
        for (const auto &page:result.pages) if (const auto *before=book.page(page.number)) {
            QHash<QString,QString> previous;
            for (const auto &word:before->words) previous.insert(word.id,word.text);
            for (const auto &word:page.words) if ((!previous.contains(word.id) || word.text!=previous[word.id]) && changes.size()<100)
                changes.append(tr("Page %1: %2 → %3").arg(page.number).arg(previous.value(word.id,tr("(unread)")),word.text));
        }
        if (!result.changes) { message->setText(tr("No unambiguous matches in recognized text. Reviewed text is preserved. Unrecognized pages can use the rules on the next OCR run.")); return; }
        QMessageBox confirm(QMessageBox::Question,tr("Apply symbol recognition"),
            tr("Apply %1 replacements on %2 pages? Changes will be saved when you save this dialog and can be undone.").arg(result.changes).arg(result.pages.size()),QMessageBox::Yes|QMessageBox::Cancel,this);
        confirm.setDetailedText(changes.join('\n')); confirm.setDefaultButton(QMessageBox::Cancel);
        if (confirm.exec()==QMessageBox::Yes) { appliedPages=result.pages; message->setText(tr("%1 replacements are ready. Save to apply them to the book.").arg(result.changes)); }
    }
}
void SymbolRulesDialog::save()
{
    for (const auto &list:{bookRules(),sharedRules()}) if (auto valid=core::readSymbolRules(core::symbolRulesJson(list));!valid) { message->setText(valid.error()); return; }
    accept();
}
void SymbolRulesDialog::reject()
{
    if (watcher.isRunning()) { closing=true; stop.request_stop(); if (cancelled) cancelled->store(true); return; }
    QDialog::reject();
}
void SymbolRulesDialog::importRules()
{
    const auto path=QFileDialog::getOpenFileName(this,tr("Import symbol rules"),{},tr("Symbol rules (*.json)")); if (path.isEmpty()) return;
    auto loaded=core::loadSymbolLibrary(path); if (!loaded) { message->setText(loaded.error()); return; }
    for (auto r:*loaded) { if (entries.size()>=core::maximumSymbolRules) break; r.id=id(); entries.append({r,scope->currentIndex()==1}); }
    appliedPages.clear(); refreshRules();
}
void SymbolRulesDialog::exportRules()
{
    auto *r=currentRule(); if (!r) return; const auto path=QFileDialog::getSaveFileName(this,tr("Export symbol rule"),{},tr("Symbol rules (*.json)")); if (path.isEmpty()) return;
    if (auto saved=core::saveSymbolLibrary({*r},path);!saved) message->setText(saved.error());
}
} // namespace yagf::ui
