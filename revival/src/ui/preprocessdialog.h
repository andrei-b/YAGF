#pragma once

#include "preprocesssettings.h"

#include <QDialog>
#include <QImage>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;

namespace yagf::ui {

class PreprocessDialog : public QDialog {
    Q_OBJECT
public:
    PreprocessDialog(const QImage &source, core::PreprocessSettings initial, QWidget *parent = nullptr);
    [[nodiscard]] core::PreprocessSettings settings() const;
private:
    void refreshPreview();

    QImage previewSource;
    QDoubleSpinBox *angleSpin;
    QCheckBox *contrastCheck;
    QCheckBox *denoiseCheck;
    QLabel *afterLabel;
    QLabel *messageLabel;
};

} // namespace yagf::ui
