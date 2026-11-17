#include "projectfile.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cassert>
#include <cstdio>

int main()
{
    QTemporaryDir directory;
    assert(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("sample.yagf.json"));
    OcrDocument original;
    original.sourcePath = QStringLiteral("/tmp/source.png");
    original.language = QStringLiteral("eng+deu");
    original.width = 200;
    original.height = 100;
    original.words.append({1, 1, 1, 1, QRect(10, 20, 30, 12),
                           QStringLiteral("world"), QStringLiteral("earth"), 82.5, true});
    QString error;
    if (!ProjectFile::save(path, original, &error)) {
        std::fprintf(stderr, "Project save failed: %s path=%s dir=%d\n", error.toUtf8().constData(),
                     path.toUtf8().constData(), QFileInfo(directory.path()).exists());
        return 1;
    }
    OcrDocument loaded;
    assert(ProjectFile::load(path, &loaded, &error));
    assert(loaded.sourcePath == original.sourcePath);
    assert(loaded.language == original.language);
    assert(loaded.words.size() == 1);
    assert(loaded.words.at(0).text == QStringLiteral("earth"));
    assert(loaded.words.at(0).originalText == QStringLiteral("world"));
    assert(loaded.words.at(0).bounds == QRect(10, 20, 30, 12));
    assert(loaded.words.at(0).reviewed);

    original.words[0].text = QStringLiteral("planet");
    assert(ProjectFile::save(path, original, &error));
    assert(ProjectFile::load(path, &loaded, &error));
    assert(loaded.words.at(0).text == QStringLiteral("planet"));

    QFile file(path);
    assert(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(QJsonObject{{"project_version", 99},
                                     {"document", original.toJson()}}).toJson());
    file.close();
    assert(!ProjectFile::load(path, &loaded, &error));
    assert(!error.isEmpty());
}
