#pragma once

#include "ocrdocument.h"

#include <QDialog>
#include <QImage>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace yagf::ui {

class LayoutDialog : public QDialog {
public:
    LayoutDialog(core::OcrPage page, const QImage &image, QWidget *parent = nullptr);
    [[nodiscard]] core::OcrPage editedPage() const { return page; }
    [[nodiscard]] bool hasChanges() const { return changed; }
private:
    void fillList(int selected);
    void selectRegion(int index);
    void applyFields();
    void moveRegion(int direction);
    void refreshPreview();

    core::OcrPage page;
    QImage previewBase;
    QListWidget *regionList;
    QLabel *previewLabel;
    QComboBox *typeCombo;
    QSpinBox *xSpin;
    QSpinBox *ySpin;
    QSpinBox *widthSpin;
    QSpinBox *heightSpin;
    QPushButton *upButton;
    QPushButton *downButton;
    bool changed = false;
};

} // namespace yagf::ui
