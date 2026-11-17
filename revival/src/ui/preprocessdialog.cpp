#include "preprocessdialog.h"
#include "imageprocessor.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace yagf::ui {

PreprocessDialog::PreprocessDialog(const QImage &source, core::PreprocessSettings initial, QWidget *parent)
    : QDialog(parent), previewSource(source.width() > 460 || source.height() > 550
        ? source.scaled(QSize(460, 550), Qt::KeepAspectRatio, Qt::SmoothTransformation) : source)
{
    setWindowTitle(tr("Improve page quality"));
    resize(1050, 710);
    auto *layout = new QVBoxLayout(this);
    const auto assessment = processing::ImageProcessor::assess(source);
    messageLabel = new QLabel(this);
    if (assessment) {
        messageLabel->setText(tr("Detected skew: %1° · contrast range: %2–%3%4")
            .arg(assessment->skewDegrees, 0, 'f', 1)
            .arg(assessment->lowLuminance).arg(assessment->highLuminance)
            .arg(assessment->needsContrast() ? tr(" · contrast improvement may help") : QString()));
    } else {
        messageLabel->setText(assessment.error());
    }
    layout->addWidget(messageLabel);

    auto *controls = new QHBoxLayout();
    angleSpin = new QDoubleSpinBox(this);
    angleSpin->setRange(-15.0, 15.0);
    angleSpin->setDecimals(1);
    angleSpin->setSingleStep(0.1);
    angleSpin->setSuffix(QStringLiteral("°"));
    angleSpin->setValue(initial.deskewDegrees);
    auto *detectButton = new QPushButton(tr("Detect skew"), this);
    connect(detectButton, &QPushButton::clicked, this, [this, assessment] {
        if (assessment) angleSpin->setValue(assessment->skewDegrees);
    });
    auto *resetButton = new QPushButton(tr("Reset"), this);
    contrastCheck = new QCheckBox(tr("Auto contrast"), this);
    contrastCheck->setChecked(initial.autoContrast);
    denoiseCheck = new QCheckBox(tr("Denoise (grayscale)"), this);
    denoiseCheck->setChecked(initial.denoise);
    controls->addWidget(new QLabel(tr("Deskew angle:"), this));
    controls->addWidget(angleSpin);
    controls->addWidget(detectButton);
    controls->addSpacing(15);
    controls->addWidget(contrastCheck);
    controls->addWidget(denoiseCheck);
    controls->addStretch();
    controls->addWidget(resetButton);
    layout->addLayout(controls);

    auto *images = new QHBoxLayout();
    auto *beforeColumn = new QVBoxLayout();
    beforeColumn->addWidget(new QLabel(tr("Original"), this));
    auto *before = new QLabel(this);
    before->setAlignment(Qt::AlignCenter);
    before->setMinimumSize(440, 540);
    before->setStyleSheet(QStringLiteral("background:white;border:1px solid #aaa"));
    before->setPixmap(QPixmap::fromImage(previewSource));
    beforeColumn->addWidget(before);
    images->addLayout(beforeColumn);
    auto *afterColumn = new QVBoxLayout();
    afterColumn->addWidget(new QLabel(tr("Preview"), this));
    afterLabel = new QLabel(this);
    afterLabel->setAlignment(Qt::AlignCenter);
    afterLabel->setMinimumSize(440, 540);
    afterLabel->setStyleSheet(QStringLiteral("background:white;border:1px solid #aaa"));
    afterColumn->addWidget(afterLabel);
    images->addLayout(afterColumn);
    layout->addLayout(images);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(angleSpin, &QDoubleSpinBox::valueChanged, this, &PreprocessDialog::refreshPreview);
    connect(contrastCheck, &QCheckBox::toggled, this, &PreprocessDialog::refreshPreview);
    connect(denoiseCheck, &QCheckBox::toggled, this, &PreprocessDialog::refreshPreview);
    connect(resetButton, &QPushButton::clicked, this, [this] {
        angleSpin->setValue(0.0);
        contrastCheck->setChecked(false);
        denoiseCheck->setChecked(false);
        refreshPreview();
    });
    refreshPreview();
}

core::PreprocessSettings PreprocessDialog::settings() const
{
    return {.deskewDegrees = angleSpin->value(),
            .autoContrast = contrastCheck->isChecked(),
            .denoise = denoiseCheck->isChecked()};
}

void PreprocessDialog::refreshPreview()
{
    const auto preview = processing::ImageProcessor::apply(previewSource, settings());
    if (!preview) {
        messageLabel->setText(preview.error());
        return;
    }
    afterLabel->setPixmap(QPixmap::fromImage(*preview).scaled(
        QSize(440, 540), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

} // namespace yagf::ui
