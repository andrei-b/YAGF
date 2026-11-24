#include "intelligencedialog.h"
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace yagf::ui {
using namespace intelligence;
namespace {
QString relationName(Relation relation, bool outgoing)
{
    switch (relation) {
    case Relation::Contains:
        return outgoing ? "Contains" : "Part of";
    case Relation::Precedes:
        return outgoing ? "Followed by" : "After";
    case Relation::Overlaps:
        return "Overlaps";
    case Relation::CaptionOf:
        return outgoing ? "Possible caption of" : "Possible caption";
    case Relation::Continues:
        return outgoing ? "May continue into" : "May continue from";
    case Relation::RefersTo:
        return outgoing ? "References" : "Referenced by";
    }
    return {};
}
}

IntelligenceDialog::IntelligenceDialog(const core::OcrDocument& document, int number, QImage image,
    const QVector<processing::OcrService>& available, const QString& areaId, int selectedWord, QWidget* parent)
    : QDialog(parent)
    , engine(document, number)
    , page(*document.page(number))
    , scan(std::move(image))
    , services(available)
{
    setObjectName("documentIntelligenceDialog");
    setWindowTitle(tr("YAGF Document Intelligence Engine"));
    resize(900, 620);
    auto* layout = new QVBoxLayout(this);
    auto* split = new QSplitter(this);
    auto* outline = new QWidget(split);
    auto* left = new QVBoxLayout(outline);
    auto* search = new QLineEdit(outline);
    search->setPlaceholderText(tr("Find a word or reading…"));
    search->setClearButtonEnabled(true);
    left->addWidget(search);
    tree = new QTreeWidget(outline);
    left->addWidget(tree);
    tree->setObjectName("intelligenceStructure");
    tree->setHeaderLabel(tr("Document structure"));
    auto* panel = new QWidget(split);
    auto* right = new QVBoxLayout(panel);
    crop = new QLabel(panel);
    crop->setAlignment(Qt::AlignCenter);
    right->addWidget(crop);
    right->addWidget(new QLabel(tr("Relations"), panel));
    links = new QListWidget(panel);
    right->addWidget(links);
    confirm = new QPushButton(tr("Confirm relation"), panel);
    confirm->setObjectName("confirmIntelligenceRelation");
    confirm->setEnabled(false);
    right->addWidget(confirm);
    right->addWidget(new QLabel(tr("Readings and evidence"), panel));
    choices = new QListWidget(panel);
    choices->setObjectName("intelligenceReadings");
    choices->setWordWrap(true);
    right->addWidget(choices);
    message = new QLabel(panel);
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    right->addWidget(message);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    use = buttons->addButton(tr("Use selected reading"), QDialogButtonBox::ActionRole);
    use->setObjectName("useIntelligenceReading");
    auto* rejectReading = buttons->addButton(tr("Reject OCR reading"), QDialogButtonBox::ActionRole);
    rejectReading->setEnabled(false);
    connect(choices, &QListWidget::currentRowChanged, this, [this, rejectReading](int row) {
        const bool valid = word >= 0 && row >= 0 && row < assessment.choices.size();
        rejectReading->setEnabled(valid && assessment.choices[row].text != page.words[word].text
            && !assessment.choices[row].runIds.isEmpty() && !assessment.choices[row].rejected);
    });
    connect(rejectReading, &QPushButton::clicked, this, [this] {
        const auto row = choices->currentRow();
        if (row < 0 || row >= assessment.choices.size())
            return;
        auto result = engine.reject(word, assessment.choices[row]);
        if (!result) {
            message->setText(result.error());
            return;
        }
        edited = std::move(*result);
        accept();
    });
    auto* askAi = buttons->addButton(tr("Review with AI…"), QDialogButtonBox::ActionRole);
    askAi->setObjectName("reviewStructureWithAi");
    const auto selectAiWords = [this](int n) {
        QVector<int> result;
        const auto& g = engine.graph;
        const auto& node = g.nodes[n];
        if (node.page != page.number)
            return result;
        if (node.kind == Kind::Page) {
            for (int i = 0; i < page.words.size(); ++i)
                result.append(i);
        } else if (node.kind == Kind::Area) {
            for (const auto& e : g.edges)
                if (e.from == n && e.relation == Relation::Overlaps && g.nodes[e.to].word >= 0)
                    result.append(g.nodes[e.to].word);
            const auto area = std::ranges::find(page.imageRegions, node.id, &core::ImageRegion::id);
            if (area != page.imageRegions.end()) {
                const auto shape = area->shape();
                for (int i = 0; i < page.words.size(); ++i)
                    if (page.words[i].geometryInferred
                        && QRegion(page.words[i].bounds).subtracted(shape).isEmpty())
                        result.append(i);
            }
        } else {
            QVector<QTreeWidgetItem*> pending { items[n] };
            while (!pending.isEmpty()) {
                const auto* item = pending.takeLast();
                const auto& child = g.nodes[item->data(0, Qt::UserRole).toInt()];
                if (child.word >= 0)
                    result.append(child.word);
                for (int j = 0; j < item->childCount(); ++j)
                    pending.append(item->child(j));
            }
        }
        std::ranges::sort(result);
        return result;
    };
    connect(tree, &QTreeWidget::currentItemChanged, this, [this, askAi, selectAiWords](QTreeWidgetItem* item) {
        aiWords = item ? selectAiWords(item->data(0, Qt::UserRole).toInt()) : QVector<int> {};
        askAi->setEnabled(!aiWords.isEmpty());
    });
    connect(askAi, &QPushButton::clicked, this, [this] {
        if (!aiWords.isEmpty())
            done(IntelligenceDialog::ReviewWithAi);
    });
    request = buttons->addButton(tr("Request OCR"), QDialogButtonBox::ActionRole);
    request->setObjectName("requestIntelligenceOcr");
    layout->addWidget(split);
    layout->addWidget(buttons);
    split->setSizes({ 300, 600 });

    const auto& g = engine.graph;
    int start = 0;
    for (int n = 0; n < g.nodes.size(); ++n) {
        const auto& node = g.nodes[n];
        auto* item = new QTreeWidgetItem(QStringList { node.label });
        item->setToolTip(0, tr("Page %1 · %2").arg(node.page).arg(node.id));
        item->setData(0, Qt::UserRole, n);
        items.append(item);
        if (node.kind == Kind::Area && node.id == areaId)
            start = n;
    }
    for (const auto& edge : g.edges)
        if (edge.relation == Relation::Contains)
            items[edge.from]->addChild(items[edge.to]);
    for (int n = 0; n < items.size(); ++n) {
        if (!items[n]->parent()) {
            if (g.nodes[n].page != number)
                items[n]->setText(0, tr("Page %1 · %2").arg(g.nodes[n].page).arg(g.nodes[n].label));
            tree->addTopLevelItem(items[n]);
        }
    }
    if (areaId.isEmpty() && selectedWord >= 0 && selectedWord < g.words.size())
        start = g.words[selectedWord];
    connect(search, &QLineEdit::returnPressed, this, [this, search] {
        const auto query = search->text().trimmed();
        if (query.isEmpty() || engine.graph.words.isEmpty())
            return;
        for (int offset = 1; offset <= engine.graph.words.size(); ++offset) {
            const int i = (word + offset) % engine.graph.words.size();
            if (page.words[i].text.contains(query, Qt::CaseInsensitive)
                || page.words[i].originalText.contains(query, Qt::CaseInsensitive)) {
                tree->setCurrentItem(items[engine.graph.words[i]]);
                tree->scrollToItem(items[engine.graph.words[i]]);
                return;
            }
        }
        message->setText(tr("No matching word on this page."));
    });
    connect(tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (item)
            selectNode(item->data(0, Qt::UserRole).toInt());
    });
    connect(links, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        const int i = item ? item->data(Qt::UserRole + 1).toInt() : -1;
        const bool editable = i >= 0 && !item->data(Qt::UserRole + 1).isNull();
        confirm->setEnabled(editable);
        if (editable)
            confirm->setText(
                engine.graph.edges[i].confirmed ? tr("Remove confirmation") : tr("Confirm relation"));
    });
    connect(confirm, &QPushButton::clicked, this, [this] {
        auto* item = links->currentItem();
        if (!item || item->data(Qt::UserRole + 1).isNull())
            return;
        const int i = item->data(Qt::UserRole + 1).toInt();
        auto result = engine.setConfirmed(i, !engine.graph.edges[i].confirmed);
        if (!result) {
            message->setText(result.error());
            return;
        }
        edited = std::move(*result);
        accept();
    });
    connect(links, &QListWidget::itemActivated, this,
        [this](QListWidgetItem* item) { tree->setCurrentItem(items[item->data(Qt::UserRole).toInt()]); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(use, &QPushButton::clicked, this, [this] {
        int row = choices->currentRow();
        if (word < 0 || row < 0 || row >= assessment.choices.size())
            return;
        auto result = engine.accept(word, assessment.choices[row]);
        if (!result) {
            message->setText(result.error());
            return;
        }
        edited = std::move(*result);
        accept();
    });
    connect(request, &QPushButton::clicked, this, [this] {
        if (compatible) {
            requestedService = compatible;
            accept();
        }
    });
    tree->expandToDepth(1);
    for (auto* button : findChildren<QPushButton*>()) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }
    tree->setCurrentItem(items[start]);
    tree->scrollToItem(items[start]);
}

void IntelligenceDialog::selectNode(int n)
{
    const auto& g = engine.graph;
    const auto& node = g.nodes[n];
    links->clear();
    choices->clear();
    crop->clear();
    message->clear();
    assessment = {};
    compatible.reset();
    word = node.word;
    for (int i = 0; i < g.edges.size(); ++i) {
        const auto& e = g.edges[i];
        if (e.from != n && e.to != n)
            continue;
        const bool outgoing = e.from == n;
        const int other = outgoing ? e.to : e.from;
        const auto& target = g.nodes[other];
        auto* item
            = new QListWidgetItem(tr("%1 · %2").arg(relationName(e.relation, outgoing), target.label), links);
        item->setToolTip(tr("Page %1 · %2").arg(target.page).arg(target.id));
        item->setData(Qt::UserRole, other);
        if (e.relation == Relation::CaptionOf || e.relation == Relation::Continues
            || e.relation == Relation::RefersTo) {
            item->setData(Qt::UserRole + 1, i);
            if (e.confirmed)
                item->setText(tr("Confirmed · %1").arg(item->text()));
        }
    }
    const bool aligned = scan.size() == QSize(page.width, page.height);
    if (aligned && node.page == page.number && scan.rect().contains(node.bounds)) {
        const auto b = node.bounds;
        const int p = std::clamp(b.height(), 10, 70);
        const QRect area = b.adjusted(-p * 2, -p, p * 2, p).intersected(scan.rect());
        const QSize target = area.size().scaled(480, 135, Qt::KeepAspectRatio);
        auto preview = scan.copy(area)
                           .scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_RGB32);
        QPainter painter(&preview);
        painter.scale(double(preview.width()) / area.width(), double(preview.height()) / area.height());
        QPen pen(QColor(197, 67, 24));
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.drawRect(b.translated(-area.topLeft()).adjusted(0, 0, -1, -1));
        painter.end();
        crop->setPixmap(QPixmap::fromImage(preview));
    }
    use->setEnabled(word >= 0);
    request->setVisible(false);
    if (word < 0)
        return;
    assessment = engine.assess(word, scan);
    int selected = 0;
    for (int i = 0; i < assessment.choices.size(); ++i) {
        const auto& h = assessment.choices[i];
        auto evidence = h.evidence;
        if (h.inkDistance)
            evidence.append(tr("ink distance %1").arg(*h.inkDistance, 0, 'f', 3));
        auto* item = new QListWidgetItem(h.text.left(100) + "  ·  " + evidence.join(" · "), choices);
        item->setToolTip(h.text + "\n" + evidence.join('\n') + "\n" + h.runIds.join('\n'));
        if (h.text == (assessment.suggested.isEmpty() ? page.words[word].text : assessment.suggested))
            selected = i;
    }
    choices->setCurrentRow(selected);
    if (!assessment.suggested.isEmpty())
        message->setText(tr("Preferred reading · %1").arg(assessment.suggested));
    else
        message->setText(assessment.choices.size() > 1
                ? tr("Unresolved. Compare the alternatives with the scan.")
                : tr("Only one reading is available."));
    if (assessment.task.isEmpty() || assessment.requestCrop.isEmpty() || !aligned)
        return;
    processing::ModelRequirements requirements;
    requirements.tasks = { assessment.task };
    QStringList keys;
    QVector<processing::OcrService> eligible;
    for (const auto& s : services) {
        if (!s.ready || !s.enabled)
            continue;
        const auto profile = s.modelProfile();
        if (!profile.incompatibilities(requirements).isEmpty())
            continue;
        const auto box = assessment.requestCrop;
        if (profile.maxImagePixels && qint64(box.width()) * box.height() > profile.maxImagePixels)
            continue;
        const auto requestedLanguage
            = s.language.isEmpty() || s.language == "auto" ? page.language : s.language;
        const auto supportsLanguage = [&](const QString& lang) {
            return std::ranges::any_of(profile.languages,[&](const QString &entry) {
                if (entry==lang || entry=="auto" || entry=="*") return true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
                const auto code=QLocale::codeToLanguage(lang);
                return code!=QLocale::AnyLanguage && code!=QLocale::C && code==QLocale::codeToLanguage(entry);
#else
                return false;
#endif
            });
        };
        if (!profile.languages.isEmpty() && !requestedLanguage.isEmpty() && requestedLanguage != "auto"
            && !std::ranges::all_of(requestedLanguage.split('+', Qt::SkipEmptyParts), supportsLanguage))
            continue;
        keys.append(s.key());
        eligible.append(s);
    }
    compatible
        = processing::selectOcrModel(eligible, requirements, engine.preferredModel(keys, assessment.task));
    if (compatible) {
        request->setText(tr("Request OCR from %1").arg(compatible->label));
        request->setVisible(true);
    } else {
        message->setText(message->text()
            + tr(" No ready %1 model is available in Recognition settings.").arg(assessment.task));
    }
}
}
