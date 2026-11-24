#pragma once

#include "correctionmemory.h"
#include "reviewchat.h"

#include <QDialog>
#include <QImage>
#include <QRect>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QPlainTextEdit;

namespace yagf::ui {

// The user verifies a contextual suggestion against enlarged source pixels.
class AiReviewDialog final : public QDialog {
public:
    AiReviewDialog(QString recognized, QString context, QImage crop, QRect target,
                   const QString &sessionToken = {}, QWidget *parent = nullptr,
                   const core::SpecialistReading *specialist = nullptr, bool passage = false);
    void setBookSuggestions(const QVector<core::CorrectionSuggestion> &items);
    void setDocumentContext(core::ReconstructionContext context);
    [[nodiscard]] QString correction() const;
    [[nodiscard]] QString sessionToken() const;

private:
    void setCandidate(const QString &text);
    void refreshPreviews();
    void askAi();
    void finishAi(ReviewChat::Answer answer);

    bool passage;
    QString secondEngine, secondReading;
    bool formula = false;
    QString recognized;
    QString context;
    core::ReconstructionContext documentContext;
    QPlainTextEdit *contextPreview = nullptr;
    QImage crop;
    QRect target;
    QImage contrast;
    QImage binary;
    QLabel *previewLabels[3]{};
    QSpinBox *zoom = nullptr;
    QLineEdit *endpoint = nullptr;
    QLineEdit *model = nullptr;
    QLineEdit *token = nullptr;
    QComboBox *bookSuggestions = nullptr;
    QLineEdit *candidate = nullptr;
    QPlainTextEdit *passageEditor = nullptr;
    QListWidget *suggestions = nullptr;
    QLabel *message = nullptr;
    QPushButton *askButton = nullptr;
    ReviewChat chat;
};

} // namespace yagf::ui
