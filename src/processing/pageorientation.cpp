#include "pageorientation.h"
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>

namespace yagf::processing {
std::expected<PageOrientation, OcrError> detectPageOrientation(const QImage &image, std::stop_token stop)
{
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    if (image.isNull() || QStandardPaths::findExecutable("tesseract").isEmpty())
        return std::unexpected(OcrError{QStringLiteral("Automatic orientation needs Tesseract with osd language data. You can rotate the page manually.")});
    QTemporaryDir directory;
    const QString path = directory.filePath("orientation.png");
    const auto input = image.width() > 2200 || image.height() > 2200
        ? image.scaled(QSize(2200,2200), Qt::KeepAspectRatio, Qt::SmoothTransformation) : image;
    if (!directory.isValid() || !input.save(path, "PNG"))
        return std::unexpected(OcrError{QStringLiteral("Could not prepare the orientation preview.")});
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    if (!environment.contains("OMP_THREAD_LIMIT")) environment.insert("OMP_THREAD_LIMIT", "2");
    process.setProcessEnvironment(environment);
    process.start("tesseract", {path, "stdout", "--psm", "0", "-l", "osd"});
    if (!process.waitForStarted(5000))
        return std::unexpected(OcrError{QStringLiteral("Could not start automatic orientation detection.")});
    QElapsedTimer timer; timer.start();
    while (process.state() != QProcess::NotRunning) {
        if (stop.stop_requested() || timer.elapsed() > 20000) {
            process.kill(); process.waitForFinished(2000);
            return std::unexpected(OcrError{QStringLiteral("Orientation detection stopped; use manual rotation."), stop.stop_requested()});
        }
        process.waitForFinished(100);
    }
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    const auto output = QString::fromUtf8(process.readAllStandardOutput());
    static const QRegularExpression rotation(QStringLiteral("(?:^|\\n)Rotate:\\s*(0|90|180|270)\\s*(?:\\n|$)"));
    static const QRegularExpression confidence(QStringLiteral("Orientation confidence:\\s*([0-9]+(?:\\.[0-9]+)?)"));
    const auto angle = rotation.match(output), certainty = confidence.match(output);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !angle.hasMatch() ||
        !certainty.hasMatch() || !std::isfinite(certainty.captured(1).toDouble()) || certainty.captured(1).toDouble() < 2.0)
        return std::unexpected(OcrError{QStringLiteral("Orientation is uncertain. Choose the rotation manually, then detect the fine skew.")});
    return PageOrientation{angle.captured(1).toInt() / 90, certainty.captured(1).toDouble()};
}
}
