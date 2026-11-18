#pragma once

#include "correctionmemory.h"

#include <QDialog>
#include <QImage>
#include <QNetworkAccessManager>
#include <QRect>
#include <QTimer>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QPlainTextEdit;
class QNetworkReply;

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
    void finishAi();

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
    QNetworkAccessManager network;
    QNetworkReply *reply = nullptr;
    QTimer deadline;
    bool timedOut = false;
};

} // namespace yagf::ui
