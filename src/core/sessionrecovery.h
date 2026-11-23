#pragma once

#include "ocrdocument.h"

#include <QLockFile>
#include <QString>
#include <memory>
#include <optional>

namespace yagf::core {

// Crash-recovery copy of a project with unsaved changes.
//
// Each running session writes its copy as <id>.yagf.json and holds <id>.lock
// next to it for as long as it runs. When the project already has a file, its
// path is kept in <id>.origin so the restored work can be saved back to it.
// A copy whose lock is no longer held was left by a session that ended without
// cleaning up (crash, kill, power loss), and the next session can restore it.
// A session that ends normally removes its copy.
class SessionRecovery {
public:
    struct Snapshot {
        QString path;
        QString originalProject; // Empty when the project had never been saved.
    };

    explicit SessionRecovery(QString directory = defaultDirectory());
    ~SessionRecovery();
    SessionRecovery(const SessionRecovery &) = delete;
    SessionRecovery &operator=(const SessionRecovery &) = delete;

    static QString defaultDirectory();

    // Newest copy left by a session that is no longer running.
    static std::optional<Snapshot> orphanedSnapshot(const QString &directory = defaultDirectory());
    static void removeSnapshot(const QString &snapshot);

    QString snapshotPath() const;
    bool save(const OcrDocument &document, const QString &originalProject, QString *error);
    void discard();

private:
    bool ensureLocked(QString *error);

    QString directory;
    QString id;
    std::unique_ptr<QLockFile> lock;
};

} // namespace yagf::core
