#include "preprocessdialog.h"
#include "imageprocessor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QtConcurrentRun>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QProgressDialog>
#include <QPushButton>
#include <QVBoxLayout>

namespace yagf::ui {

PreprocessDialog::PreprocessDialog(const QImage &source, core::PreprocessSettings initial, QWidget *parent, QString language)
    : QDialog(parent), pilotLanguage(std::move(language)), inkThreshold(initial.inkThreshold), previewSource(source.width() > 460 || source.height() > 550
        ? source.scaled(QSize(460, 550), Qt::KeepAspectRatio, Qt::SmoothTransformation) : source)
{
    analysisSource = source.width() > 2200 || source.height() > 2200
        ? source.scaled(QSize(2200,2200), Qt::KeepAspectRatio, Qt::SmoothTransformation) : source;
    setObjectName(QStringLiteral("preprocessDialog"));
    setWindowTitle(tr("Improve page quality"));
    resize(1050, 710);
    auto *layout = new QVBoxLayout(this);
    messageLabel = new QLabel(this);
    messageLabel->setWordWrap(true);
    messageLabel->setText(tr("For uneven lighting, skew detection uses a locally binarized copy. Rotation and binarization can also be applied to the OCR image."));
    layout->addWidget(messageLabel);
    auto *controls = new QHBoxLayout();
    rotationCombo = new QComboBox(this);
    rotationCombo->setObjectName(QStringLiteral("pageRotation"));
    rotationCombo->addItems({tr("No rotation"), tr("90° clockwise"), tr("180°"), tr("90° counterclockwise")});
    rotationCombo->setCurrentIndex(initial.quarterTurns);
    angleSpin = new QDoubleSpinBox(this);
    angleSpin->setObjectName(QStringLiteral("deskewAngle"));
    angleSpin->setRange(-15.0, 15.0);
    angleSpin->setDecimals(1);
    angleSpin->setSingleStep(0.1);
    angleSpin->setSuffix(QStringLiteral("°"));
    angleSpin->setValue(initial.deskewDegrees);
    auto *detectButton = new QPushButton(tr("Detect skew"), this);
    connect(detectButton, &QPushButton::clicked, this, &PreprocessDialog::detectSkew);
    orientationButton = new QPushButton(tr("Detect orientation + skew"), this);
    connect(orientationButton, &QPushButton::clicked, this, &PreprocessDialog::detectOrientation);
    controls->addWidget(rotationCombo);
    controls->addWidget(new QLabel(tr("Fine skew:"), this));
    controls->addWidget(angleSpin);
    controls->addWidget(detectButton);
    controls->addWidget(orientationButton);
    auto *textButton = new QPushButton(tr("Analyze text and ink"), this);
    textButton->setObjectName(QStringLiteral("analyzeText"));
    textButton->setToolTip(tr("Run a local Tesseract pilot to estimate fine skew and separate text strokes from paper."));
    controls->addWidget(textButton);
    connect(textButton, &QPushButton::clicked, this, &PreprocessDialog::analyzeText);
    controls->addStretch();
    layout->addLayout(controls);
    auto *filters = new QHBoxLayout();
    auto *resetButton = new QPushButton(tr("Reset"), this);
    contrastCheck = new QCheckBox(tr("Auto contrast"), this);
    contrastCheck->setChecked(initial.autoContrast);
    denoiseCheck = new QCheckBox(tr("Denoise (grayscale)"), this);
    denoiseCheck->setChecked(initial.denoise);
    binaryCheck = new QCheckBox(tr("Adaptive binarization"), this);
    binaryCheck->setObjectName(QStringLiteral("adaptiveBinarization"));
    binaryCheck->setToolTip(tr("Remove uneven paper lighting with local black-and-white thresholding. This changes the OCR image; skew detection uses its own binary copy regardless."));
    binaryCheck->setChecked(initial.adaptiveBinarization);
    filters->addWidget(contrastCheck);
    filters->addWidget(denoiseCheck);
    filters->addWidget(binaryCheck);
    filters->addStretch();
    filters->addWidget(resetButton);
    layout->addLayout(filters);

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
    connect(binaryCheck, &QCheckBox::toggled, this, &PreprocessDialog::refreshPreview);
    connect(rotationCombo, &QComboBox::currentIndexChanged, this, [this] { angleSpin->setValue(0); refreshPreview(); });
    connect(angleSpin, &QDoubleSpinBox::valueChanged, this, &PreprocessDialog::refreshPreview);
    const auto changedFilter = [this] { inkThreshold = 0; refreshPreview(); };
    connect(contrastCheck, &QCheckBox::toggled, this, changedFilter);
    connect(denoiseCheck, &QCheckBox::toggled, this, changedFilter);
    connect(resetButton, &QPushButton::clicked, this, [this] {
        inkThreshold = 0;
        rotationCombo->setCurrentIndex(0);
        binaryCheck->setChecked(false);
        angleSpin->setValue(0.0);
        contrastCheck->setChecked(false);
        denoiseCheck->setChecked(false);
        refreshPreview();
    });
    connect(&orientationWatcher, &QFutureWatcherBase::finished, this, [this] {
        if (orientationStop.stop_requested()) return;
        orientationButton->setEnabled(true);
        const auto result = orientationWatcher.result();
        if (result) rotationCombo->setCurrentIndex(result->quarterTurns);
        detectSkew();
        if (!result && !result.error().cancelled)
            messageLabel->setText(result.error().message + tr(" Fine skew at the selected rotation: %1°.").arg(angleSpin->value(),0,'f',1));
    });
    connect(&textWatcher, &QFutureWatcherBase::finished, this, [this] {
        if (textStop.stop_requested()) return;
        const auto result = textWatcher.result();
        if (!result) { messageLabel->setText(result.error().message); return; }
        if (result->skewDegrees) angleSpin->setValue(*result->skewDegrees);
        if (result->inkThreshold) {
            inkThreshold = result->inkThreshold;
            binaryCheck->setChecked(true);
        }
        messageLabel->setText(tr("%1 confident words. ").arg(result->wordCount)
            + (result->skewDegrees ? tr("Skew %1° from %2 lines. ").arg(angleSpin->value()).arg(result->lineCount)
                                   : tr("Insufficient line agreement to change skew. "))
            + (result->inkThreshold ? tr("Ink separation estimated from word pixels.")
                                    : tr("Insufficient ink samples to change binarization.")));
        refreshPreview();
    });
    refreshPreview();
}

PreprocessDialog::~PreprocessDialog()
{
    textStop.request_stop();
    if (textWatcher.isRunning()) textWatcher.waitForFinished();
    orientationStop.request_stop();
    if (orientationWatcher.isRunning()) orientationWatcher.waitForFinished();
}

void PreprocessDialog::detectSkew()
{
    const auto rotated = processing::ImageProcessor::apply(analysisSource, {.quarterTurns = rotationCombo->currentIndex()});
    if (!rotated) { messageLabel->setText(rotated.error()); return; }
    const auto assessment = processing::ImageProcessor::assess(*rotated);
    if (!assessment) { messageLabel->setText(assessment.error()); return; }
    angleSpin->setValue(assessment->skewDegrees);
    messageLabel->setText(tr("Detected fine skew: %1° after %2° clockwise rotation. Detection used local binarization to suppress shadows.")
        .arg(assessment->skewDegrees,0,'f',1).arg(rotationCombo->currentIndex()*90));
}

void PreprocessDialog::analyzeText()
{
    if (textWatcher.isRunning() || orientationWatcher.isRunning()) return;
    auto *progress = new QProgressDialog(tr("Recognizing sample words and measuring text lines…"),
                                         tr("Cancel"), 0, 0, this);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    textStop = std::stop_source{};
    connect(progress, &QProgressDialog::canceled, this, [this] { textStop.request_stop(); });
    connect(&textWatcher, &QFutureWatcherBase::finished, progress, &QWidget::close);
    const auto input = analysisSource;
    auto filters = settings();
    filters.deskewDegrees = 0;
    filters.adaptiveBinarization = false;
    textWatcher.setFuture(QtConcurrent::run([input, filters, language = pilotLanguage, stop = textStop.get_token()] {
        const auto pilot = processing::ImageProcessor::apply(input, filters);
        if (!pilot) return std::expected<processing::TextGuidance, processing::OcrError>(
            std::unexpected(processing::OcrError{pilot.error()}));
        return processing::analyzeTextPilot(*pilot, language, stop);
    }));
    progress->show();
}

void PreprocessDialog::detectOrientation()
{
    if (orientationWatcher.isRunning()) return;
    orientationButton->setEnabled(false);
    messageLabel->setText(tr("Detecting text orientation…"));
    orientationStop = std::stop_source{};
    const auto token = orientationStop.get_token();
    const QImage source = analysisSource;
    orientationWatcher.setFuture(QtConcurrent::run([source, token] {
        const auto binary = processing::ImageProcessor::apply(source, {.adaptiveBinarization = true});
        if (!binary) return std::expected<processing::PageOrientation,processing::OcrError>(std::unexpected(processing::OcrError{binary.error()}));
        return processing::detectPageOrientation(*binary, token);
    }));
}

core::PreprocessSettings PreprocessDialog::settings() const
{
    return {.deskewDegrees = angleSpin->value(),
            .autoContrast = contrastCheck->isChecked(),
            .denoise = denoiseCheck->isChecked(),
            .quarterTurns = rotationCombo->currentIndex(),
            .adaptiveBinarization = binaryCheck->isChecked(),
            .inkThreshold = binaryCheck->isChecked() ? inkThreshold : 0};
}

void PreprocessDialog::refreshPreview()
{
    const auto preview = processing::ImageProcessor::apply(inkThreshold && binaryCheck->isChecked() ? analysisSource : previewSource, settings());
    if (!preview) {
        messageLabel->setText(preview.error());
        return;
    }
    afterLabel->setPixmap(QPixmap::fromImage(*preview).scaled(
        QSize(440, 540), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

} // namespace yagf::ui
