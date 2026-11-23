#include "sessionrecovery.h"
#include "projectfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace yagf::core {
namespace {
const QString snapshotSuffix = QStringLiteral(".yagf.json");

QString companionPath(const QString &snapshot, const QString &suffix)
{
    return snapshot.left(snapshot.size() - snapshotSuffix.size()) + suffix;
}

QString lockPathFor(const QString &snapshot)
{
    return companionPath(snapshot, QStringLiteral(".lock"));
}

QString originPathFor(const QString &snapshot)
{
    return companionPath(snapshot, QStringLiteral(".origin"));
}

QString readOrigin(const QString &snapshot)
{
    QFile origin(originPathFor(snapshot));
    if (!origin.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(origin.readAll()).trimmed();
}
}

SessionRecovery::SessionRecovery(QString recoveryDirectory)
    : directory(std::move(recoveryDirectory)), id(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

SessionRecovery::~SessionRecovery()
{
    discard();
}

QString SessionRecovery::defaultDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/recovery");
}

std::optional<SessionRecovery::Snapshot> SessionRecovery::orphanedSnapshot(const QString &directory)
{
    const auto snapshots = QDir(directory).entryInfoList({QLatin1Char('*') + snapshotSuffix},
                                                        QDir::Files, QDir::Time);
    for (const QFileInfo &snapshot : snapshots) {
        // A live session keeps its lock; tryLock succeeds only when the owner is gone.
        // Time-based staleness is disabled so long sessions are never mistaken for dead ones.
        QLockFile probe(lockPathFor(snapshot.absoluteFilePath()));
        probe.setStaleLockTime(0);
        if (!probe.tryLock(0)) continue;
        probe.unlock();
        if (snapshot.size() > 0) return Snapshot{snapshot.absoluteFilePath(), readOrigin(snapshot.absoluteFilePath())};
        removeSnapshot(snapshot.absoluteFilePath());
    }
    return std::nullopt;
}

void SessionRecovery::removeSnapshot(const QString &snapshot)
{
    QFile::remove(snapshot);
    QFile::remove(originPathFor(snapshot));
    QFile::remove(lockPathFor(snapshot));
}

QString SessionRecovery::snapshotPath() const
{
    return QDir(directory).filePath(id + snapshotSuffix);
}

bool SessionRecovery::ensureLocked(QString *error)
{
    if (lock) return true;
    if (!QDir().mkpath(directory)) {
        if (error) *error = QStringLiteral("Cannot create the recovery folder %1").arg(directory);
        return false;
    }
    auto candidate = std::make_unique<QLockFile>(lockPathFor(snapshotPath()));
    candidate->setStaleLockTime(0);
    if (!candidate->tryLock(0)) {
        if (error) *error = QStringLiteral("Cannot lock the recovery copy %1").arg(snapshotPath());
        return false;
    }
    lock = std::move(candidate);
    return true;
}

bool SessionRecovery::save(const OcrDocument &document, const QString &originalProject, QString *error)
{
    if (!ensureLocked(error) || !ProjectFile::save(snapshotPath(), document, error)) return false;
    const QString originPath = originPathFor(snapshotPath());
    if (originalProject.isEmpty()) {
        QFile::remove(originPath);
        return true;
    }
    QSaveFile origin(originPath);
    if (!origin.open(QIODevice::WriteOnly) ||
        origin.write(QFileInfo(originalProject).absoluteFilePath().toUtf8()) < 0 || !origin.commit()) {
        if (error) *error = origin.errorString();
        return false;
    }
    return true;
}

void SessionRecovery::discard()
{
    if (!lock) return;
    QFile::remove(snapshotPath());
    QFile::remove(originPathFor(snapshotPath()));
    lock->unlock();
    lock.reset();
}

} // namespace yagf::core
