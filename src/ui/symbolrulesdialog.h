#pragma once
#include "ocrdocument.h"
#include <QDialog>
#include <QFutureWatcher>
#include <QImage>
#include <atomic>
#include <functional>
#include <memory>
#include <stop_token>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QLabel;
class QSpinBox;
class QProgressBar;
class QPushButton;
class QDialogButtonBox;

namespace yagf::ui {
class SymbolCropView;
struct SymbolRuleEntry { core::SymbolRule rule; bool shared = false; };
struct SymbolRuleWork {
    QString error;
    QVector<core::SymbolExample> examples;
    QVector<core::OcrPage> pages;
    QJsonObject model;
    QImage image;
    QRect crop;
    int scanned = 0, changes = 0, proposed = 0;
    bool cancelled = false;
};

class SymbolRulesDialog final : public QDialog {
public:
    SymbolRulesDialog(core::OcrDocument book, QVector<core::SymbolRule> shared, QWidget *parent = nullptr,
                      QImage seed = {}, QRect seedBounds = {}, int seedPage = 1, QString selected = {},
                      int wordIndex = -1, int characterStart = -1, int characterLength = 0);
    ~SymbolRulesDialog() override;
    QVector<core::SymbolRule> bookRules() const;
    QVector<core::SymbolRule> sharedRules() const;
    QVector<core::OcrPage> changedPages() const { return appliedPages; }
    void reject() override;
private:
    void showRule(int index);
    void refreshRules();
    void refreshExamples();
    void showExample(int index);
    void setDecision(const QString &decision);
    void commitCrop();
    void mine();
    void train();
    void apply();
    void refine();
    void save();
    void importRules();
    void exportRules();
    void run(QString kind, std::function<SymbolRuleWork(std::stop_token, const std::atomic_bool &)> task);
    void finished();
    void progress(int page, int count);
    core::SymbolRule *currentRule();

    core::OcrDocument book;
    QString sourceIdentity;
    QVector<SymbolRuleEntry> entries;
    QVector<core::OcrPage> appliedPages;
    QComboBox *rules = nullptr, *scope = nullptr, *backend = nullptr;
    QLineEdit *label = nullptr, *language = nullptr, *endpoint = nullptr, *token = nullptr;
    QCheckBox *enabled = nullptr;
    QListWidget *examples = nullptr;
    QLabel *message = nullptr, *state = nullptr, *preview = nullptr;
    QSpinBox *first = nullptr, *last = nullptr;
    QProgressBar *bar = nullptr;
    QPushButton *cancel = nullptr;
    QDialogButtonBox *buttons = nullptr;
    QWidget *panel = nullptr;
    SymbolCropView *crop = nullptr;
    QFutureWatcher<SymbolRuleWork> watcher;
    std::stop_source stop;
    std::shared_ptr<std::atomic_bool> cancelled;
    QString kind;
    bool showing = false, closing = false, seedCrop = false;
    int editingExample = -1;
};
} // namespace yagf::ui
