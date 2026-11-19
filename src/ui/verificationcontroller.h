#pragma once
#include "verificationqueue.h"
#include "spellchecker.h"
#include <QObject>
#include <QTimer>
#include <optional>
class QDockWidget;
class QListView;
class QComboBox;
class QCheckBox;
class QLabel;
class QMainWindow;
namespace yagf::ui {
class VerificationListModel;
// Owns queue presentation and filtering, not document mutations or page rendering.
class VerificationController final : public QObject {
    Q_OBJECT
public:
    VerificationController(QMainWindow *window, const core::OcrDocument &document);
    void invalidate(int currentPage);
    void setBusy(bool busy);
    void show();
    void navigate(int direction);
signals:
    void activateRequested(const core::VerificationItem &item);
    void reviewRequested(const core::VerificationItem &item);
    void verifyRequested(const core::VerificationItem &item);
    void discardRequested(const core::VerificationItem &item);
private:
    void refresh();
    void filter();
    std::optional<core::VerificationItem> selected() const;
    const core::OcrDocument &document;
    core::SpellChecker spelling;
    core::VerificationQueue queue;
    QDockWidget *dock;
    QListView *list;
    VerificationListModel *model;
    QComboBox *reason;
    QCheckBox *pageOnly;
    QLabel *counts;
    QTimer timer;
    int currentPage = 1;
    int resumeRow = 0;
    bool dirty = true;
    bool busy = false;
};
}
