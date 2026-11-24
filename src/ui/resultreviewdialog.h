#pragma once
#include "resultreview.h"
#include "reviewchat.h"
#include <QDialog>
#include <optional>

class QCheckBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace yagf::ui {
class ResultReviewDialog final : public QDialog {
public:
    ResultReviewDialog(const core::OcrPage& source, QVector<int> words, QImage scan, bool unicode,
        const QString& sessionToken, QWidget* parent);
    QString sessionToken() const;
    std::optional<core::OcrPage> edited;

private:
    void sendBatch();
    void setWorking(bool active);
    void updateApply();
    core::OcrPage page;
    QImage image;
    bool unicode;
    QVector<intelligence::ReviewBatch> batches;
    QVector<intelligence::ReviewChange> changes;
    int batchIndex = 0;
    QString usedModel, usedEndpoint, nearby;
    bool usedImage = false;
    ReviewChat chat;
    QLineEdit *endpoint, *model, *token;
    QCheckBox* withImage;
    QLabel *status, *preview;
    QPushButton *ask, *apply;
    QTreeWidget* list;
};
}
