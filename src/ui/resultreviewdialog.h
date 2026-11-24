#pragma once
#include "resultreview.h"
#include "reviewchat.h"
#include <QDialog>
#include <optional>

class QCheckBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QScrollArea;
class QTextBrowser;
class QVBoxLayout;

namespace yagf::ui {
class ResultReviewDialog final : public QDialog {
public:
    ResultReviewDialog(const core::OcrPage& source, QVector<int> words, QImage scan, bool unicode,
        const QString& sessionToken, QWidget* parent);
    QString sessionToken() const;
    std::optional<core::OcrPage> edited;

private:
    struct Note {
        QCheckBox *check;
        QWidget *card;
        int textStart = 0;
        int textEnd = 0;
    };

    void sendBatch();
    void setWorking(bool active);
    void updateApply();
    void appendNote(int index);
    void focusNote(int index, bool scrollToText);
    void renderTranscription();
    void updateHighlights();
    core::OcrPage page;
    QImage image;
    bool unicode;
    QVector<intelligence::ReviewBatch> batches;
    QVector<intelligence::ReviewChange> changes;
    QVector<Note> notes;
    int batchIndex = 0;
    int activeNote = -1;
    QString usedModel, usedEndpoint, nearby;
    bool usedImage = false;
    bool usedDirectAnswer = false;
    ReviewChat chat;
    QLineEdit *endpoint, *model, *token;
    QCheckBox *withImage, *directAnswer;
    QLabel *status, *preview, *noteCount, *emptyNotes;
    QWidget *scanPanel;
    QPushButton *ask, *apply;
    QTextBrowser *transcription;
    QScrollArea *margin;
    QVBoxLayout *noteLayout;
};
}
