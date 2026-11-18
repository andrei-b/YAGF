#include "exportcommand.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>

namespace yagf::processing::detail {
namespace {
ExportResult failure(const QString &error)
{
    return {.status = ExportResult::Status::Error, .error = error};
}

}

ExportResult runExportCommand(const QString &program, const QStringList &arguments,
                           const std::atomic_bool &cancelled)
{
    if (QStandardPaths::findExecutable(program).isEmpty())
        return failure(QStringLiteral("%1 is not installed or is not on PATH.").arg(program));
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(10'000))
        return failure(QStringLiteral("Could not start %1: %2").arg(program, process.errorString()));
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning) {
        if (cancelled.load()) {
            process.kill();
            process.waitForFinished(2000);
            return {.status = ExportResult::Status::Cancelled};
        }
        if (timer.elapsed() > 300'000) {
            process.kill();
            process.waitForFinished(2000);
            return failure(QStringLiteral("%1 timed out.").arg(program));
        }
        process.waitForFinished(100);
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return failure(QStringLiteral("%1 failed: %2").arg(program,
            QString::fromUtf8(process.readAllStandardError()).trimmed()));
    return {.status = ExportResult::Status::Success};
}

} // namespace yagf::processing::detail
