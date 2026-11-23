#include "projectfile.h"
#include "sessionrecovery.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSysInfo>
#include <QTemporaryDir>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf::core;

namespace {
OcrDocument sampleDocument()
{
    OcrDocument document;
    document.sourcePath = QStringLiteral("/tmp/book.pdf");
    document.totalPages = 3;
    for (int number : {1, 2}) {
        OcrPage page;
        page.number = number;
        page.width = 200;
        page.height = 100;
        page.words.append({1, 1, 1, 1, QRect(10, 20, 30, 12), QStringLiteral("page%1").arg(number),
                           QStringLiteral("page%1").arg(number), 90, false});
        document.replacePage(std::move(page));
    }
    return document;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    assert(directory.isValid());
    const QString root = directory.filePath(QStringLiteral("recovery"));
    QString error;

    // Nothing is written until the first save, and a live session's copy is never offered.
    {
        SessionRecovery live(root);
        assert(!QFileInfo::exists(live.snapshotPath()));
        assert(live.save(sampleDocument(), {}, &error));
        assert(QFileInfo::exists(live.snapshotPath()));
        assert(!SessionRecovery::orphanedSnapshot(root));
        OcrDocument restored;
        assert(ProjectFile::load(live.snapshotPath(), &restored, &error));
        assert(restored.pages.size() == 2 && restored.page(2)->words.first().text == QStringLiteral("page2"));
    }
    // A session that ends normally leaves nothing behind.
    assert(!SessionRecovery::orphanedSnapshot(root));
    assert(QDir(root).entryList(QDir::Files).isEmpty());

    // Discarding (project saved or abandoned) removes the copy.
    {
        SessionRecovery live(root);
        assert(live.save(sampleDocument(), {}, &error));
        live.discard();
        assert(!QFileInfo::exists(live.snapshotPath()));
    }

    // A copy whose lock names a process that no longer exists is offered once.
    const QString crashed = QDir(root).filePath(QStringLiteral("crashed.yagf.json"));
    assert(ProjectFile::save(crashed, sampleDocument(), &error));
    QFile staleLock(QDir(root).filePath(QStringLiteral("crashed.lock")));
    assert(staleLock.open(QIODevice::WriteOnly));
    staleLock.write("2147483000\nyagf2\n" + QSysInfo::machineHostName().toUtf8() + "\n");
    staleLock.close();
    const auto orphan = SessionRecovery::orphanedSnapshot(root);
    assert(orphan && QFileInfo(orphan->path) == QFileInfo(crashed) && orphan->originalProject.isEmpty());
    SessionRecovery::removeSnapshot(orphan->path);
    assert(!SessionRecovery::orphanedSnapshot(root));

    // Changes to a saved project remember that project, so they can be saved back to it.
    const QString savedProject = directory.filePath(QStringLiteral("book.yagf.json"));
    QString leftSnapshot;
    {
        SessionRecovery live(root);
        assert(live.save(sampleDocument(), savedProject, &error));
        leftSnapshot = live.snapshotPath();
        assert(QFileInfo::exists(QString(leftSnapshot).replace(QStringLiteral(".yagf.json"), QStringLiteral(".origin"))));
        // Saving again as an untitled project forgets the origin.
        assert(live.save(sampleDocument(), {}, &error));
        assert(!QFileInfo::exists(QString(leftSnapshot).replace(QStringLiteral(".yagf.json"), QStringLiteral(".origin"))));
        assert(live.save(sampleDocument(), savedProject, &error));
        // Simulate a crash: the copy and its origin stay, the lock goes with the process.
        QFile::copy(leftSnapshot, leftSnapshot + QStringLiteral(".keep"));
        QFile::copy(QString(leftSnapshot).replace(QStringLiteral(".yagf.json"), QStringLiteral(".origin")),
                    leftSnapshot + QStringLiteral(".origin.keep"));
    }
    QFile::rename(leftSnapshot + QStringLiteral(".keep"), leftSnapshot);
    QFile::rename(leftSnapshot + QStringLiteral(".origin.keep"),
                  QString(leftSnapshot).replace(QStringLiteral(".yagf.json"), QStringLiteral(".origin")));
    const auto savedOrphan = SessionRecovery::orphanedSnapshot(root);
    assert(savedOrphan && savedOrphan->path == QFileInfo(leftSnapshot).absoluteFilePath());
    assert(savedOrphan->originalProject == QFileInfo(savedProject).absoluteFilePath());
    SessionRecovery::removeSnapshot(savedOrphan->path);
    assert(QDir(root).entryList(QDir::Files).isEmpty());

    // A copy whose lock file disappeared is also orphaned; empty copies are dropped.
    assert(ProjectFile::save(crashed, sampleDocument(), &error));
    assert(SessionRecovery::orphanedSnapshot(root));
    SessionRecovery::removeSnapshot(crashed);
    QFile empty(QDir(root).filePath(QStringLiteral("empty.yagf.json")));
    assert(empty.open(QIODevice::WriteOnly));
    empty.close();
    assert(!SessionRecovery::orphanedSnapshot(root));
    assert(!QFileInfo::exists(empty.fileName()));
    return 0;
}
