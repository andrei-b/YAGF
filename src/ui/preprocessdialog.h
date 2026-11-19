#pragma once

#include "preprocesssettings.h"
#include "pageorientation.h"
#include "textguidedanalysis.h"
#include <QFutureWatcher>
#include <stop_token>

#include <QDialog>
#include <QImage>

class QCheckBox;
class QDoubleSpinBox;
class QComboBox;
class QPushButton;
class QLabel;

namespace yagf::ui {

class PreprocessDialog : public QDialog {
    Q_OBJECT
public:
    PreprocessDialog(const QImage &source, core::PreprocessSettings initial, QWidget *parent = nullptr,
                     QString pilotLanguage = QStringLiteral("eng"));
    ~PreprocessDialog() override;
    [[nodiscard]] core::PreprocessSettings settings() const;
private:
    void refreshPreview();
    void detectSkew();
    void detectOrientation();
    void analyzeText();
    QString pilotLanguage;
    int inkThreshold;
    QFutureWatcher<std::expected<processing::TextGuidance, processing::OcrError>> textWatcher;
    std::stop_source textStop;

    QImage previewSource, analysisSource;
    QComboBox *rotationCombo;
    QCheckBox *binaryCheck;
    QPushButton *orientationButton;
    QFutureWatcher<std::expected<processing::PageOrientation, processing::OcrError>> orientationWatcher;
    std::stop_source orientationStop;
    QDoubleSpinBox *angleSpin;
    QCheckBox *contrastCheck;
    QCheckBox *denoiseCheck;
    QLabel *afterLabel;
    QLabel *messageLabel;
};

} // namespace yagf::ui
