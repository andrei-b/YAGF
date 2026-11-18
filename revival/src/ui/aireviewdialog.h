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
class QNetworkReply;

namespace yagf::ui {

// The user verifies a contextual suggestion against enlarged source pixels.
class AiReviewDialog final : public QDialog {
public:
    AiReviewDialog(QString recognized, QString context, QImage crop, QRect target,
                   QString sessionToken = {}, QWidget *parent = nullptr);
    void setBookSuggestions(const QVector<core::CorrectionSuggestion> &items);
    [[nodiscard]] QString correction() const;
    [[nodiscard]] QString sessionToken() const;

private:
    void refreshPreviews();
    void askAi();
    void finishAi();

    QString recognized;
    QString context;
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
    QListWidget *suggestions = nullptr;
    QLabel *message = nullptr;
    QPushButton *askButton = nullptr;
    QNetworkAccessManager network;
    QNetworkReply *reply = nullptr;
    QTimer deadline;
    bool timedOut = false;
};

} // namespace yagf::ui
