#pragma once
#include "ocrreadings.h"
#include <QDialog>
#include <QImage>
class QListWidget;
class QComboBox;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QCheckBox;
namespace yagf::ui {
class OcrReadingsDialog final:public QDialog {
    Q_OBJECT
public:
    OcrReadingsDialog(core::OcrPage page,QImage image,QWidget *parent=nullptr);
    const core::OcrPage &editedPage() const {return page;}
    bool hasChanges() const {return changed;}
private:
    void rebuild(int row=0);
    void showSpan();
    void showReading();
    void useReading();
    core::OcrPage page;
    QImage image;
    QVector<QVector<int>> spans;
    QListWidget *lines;
    QComboBox *engines;
    QLabel *preview,*status;
    QPlainTextEdit *primaryText,*alternative;
    QPushButton *use;
    QCheckBox *complete;
    QString matching;
    bool changed=false;
};
}
