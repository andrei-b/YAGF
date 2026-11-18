#include "projectfile.h"

#include <QFile>
#include <QJsonArray>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>

using namespace yagf::core;

int main()
{
    QTemporaryDir directory;
    assert(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("sample.yagf.json"));
    OcrDocument original;
    original.sourcePath = QStringLiteral("/tmp/source.png");
    original.language = QStringLiteral("eng+deu");
    original.context = {QStringLiteral("A physics textbook, English and German."), false};
    OcrPage page;
    page.number = 1;
    page.width = 200;
    page.height = 100;
    page.words.append({1, 1, 1, 1, QRect(10, 20, 30, 12),
                           QStringLiteral("world"), QStringLiteral("earth"), 82.5, true});
    original.replacePage(std::move(page));
    original.page(1)->words[0].bold = true;
    original.page(1)->words[0].italic = true;
    original.page(1)->words[0].fontSizePt = 15;
    original.page(1)->preprocessing = {.deskewDegrees = 1.5, .autoContrast = true, .denoise = false};
    original.page(1)->sourceWidth = 198;
    original.page(1)->sourceHeight = 98;
    original.page(1)->imageRegions = {{QStringLiteral("selected-area"), QRect(5, 8, 95, 60)}};
    OcrPage secondPage;
    secondPage.number = 2;
    secondPage.width = 200;
    secondPage.height = 100;
    secondPage.words.append({2, 1, 1, 1, QRect(20, 30, 40, 12),
                             QStringLiteral("Hallo"), QStringLiteral("Hallo!"), 90, true});
    original.totalPages = 2;
    original.replacePage(std::move(secondPage));
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
    assert(loaded.context == original.context);
    assert(loaded.page(1)->words.size() == 1);
    assert(loaded.page(1)->words.at(0).text == QStringLiteral("earth"));
    assert(loaded.page(1)->words.at(0).originalText == QStringLiteral("world"));
    assert(loaded.page(1)->words.at(0).bounds == QRect(10, 20, 30, 12));
    assert(loaded.page(1)->words.at(0).reviewed);
    assert(loaded.page(1)->words.at(0).bold);
    assert(loaded.page(1)->words.at(0).italic);
    assert(loaded.page(1)->words.at(0).fontSizePt == 15);
    assert(loaded.page(1)->words.at(0).sourceFontSizePt > 0);
    assert(loaded.page(1)->preprocessing.deskewDegrees == 1.5);
    assert(loaded.page(1)->preprocessing.autoContrast);
    assert(loaded.page(1)->sourceWidth == 198);
    assert(loaded.page(1)->imageRegions.size() == 1);
    assert(loaded.page(1)->imageRegions.first().bounds == QRect(5, 8, 95, 60));
    assert(loaded.totalPages == 2);
    assert(loaded.page(2)->words.at(0).text == QStringLiteral("Hallo!"));
    assert(loaded.page(2)->regions.at(0).paragraphs.at(0).lines.at(0).wordIds.size() == 1);

    original.page(1)->words[0].text = QStringLiteral("planet");
    assert(ProjectFile::save(path, original, &error));
    assert(ProjectFile::load(path, &loaded, &error));
    assert(loaded.page(1)->words.at(0).text == QStringLiteral("planet"));

    QFile file(path);
    assert(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(QJsonObject{{"project_version", 99},
                                     {"document", original.toJson()}}).toJson());
    file.close();
    assert(!ProjectFile::load(path, &loaded, &error));
    assert(!error.isEmpty());

    const QJsonObject legacyWord{{"page", 1}, {"block", 1}, {"paragraph", 1}, {"line", 1},
        {"box", QJsonObject{{"x", 10}, {"y", 20}, {"width", 30}, {"height", 12}}},
        {"original_text", "old"}, {"text", "fixed"}, {"confidence", 80}, {"reviewed", true}};
    const QJsonObject legacyDocument{{"schema_version", 1}, {"source", "/tmp/source.png"},
        {"engine", "tesseract"}, {"language", "eng"},
        {"page", QJsonObject{{"width", 200}, {"height", 100}}},
        {"words", QJsonArray{legacyWord}}};
    assert(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(QJsonObject{{"project_version", 1}, {"document", legacyDocument}}).toJson());
    file.close();
    assert(ProjectFile::load(path, &loaded, &error));
    assert(loaded.page(1)->words.at(0).text == QStringLiteral("fixed"));
    assert(!loaded.page(1)->words.at(0).id.isEmpty());
    assert(loaded.context.description.isEmpty() && loaded.context.inferFromRecognizedText);

    // Older v2 projects omit context; malformed new fields must not be silently accepted.
    QJsonObject oldDocument = original.toJson();
    oldDocument.remove("context");
    assert(OcrDocument::fromJson(oldDocument, &loaded, &error));
    assert(loaded.context == DocumentContext{});
    for (const QJsonValue &invalid : {QJsonValue("bad"), QJsonValue(QJsonObject{{"description", 42},
            {"infer_from_recognized_text", true}}), QJsonValue(QJsonObject{{"description", "valid"},
            {"infer_from_recognized_text", "yes"}}), QJsonValue(QJsonObject{
            {"description", QString(DocumentContext::maximumDescriptionLength + 1, 'x')},
            {"infer_from_recognized_text", true}})}) {
        auto broken = original.toJson();
        broken.insert("context", invalid);
        assert(!OcrDocument::fromJson(broken, &loaded, &error));
    }

    const QString sourcePath = directory.filePath(QStringLiteral("portable-source.png"));
    QFile source(sourcePath);
    assert(source.open(QIODevice::WriteOnly));
    const QByteArray imageBytes("portable image content");
    assert(source.write(imageBytes) == imageBytes.size());
    source.close();
    OcrDocument portable = original;
    portable.sourcePath = sourcePath;
    const QString bundle = directory.filePath(QStringLiteral("portable.yagfproj"));
    assert(ProjectFile::savePortable(bundle, portable, &error));
    QFile manifest(QDir(bundle).filePath(QStringLiteral("project.yagf.json")));
    assert(manifest.open(QIODevice::ReadOnly));
    const QString storedSource = QJsonDocument::fromJson(manifest.readAll()).object()
        .value(QStringLiteral("document")).toObject().value(QStringLiteral("source")).toString();
    assert(storedSource.startsWith(QStringLiteral("assets/")));
    manifest.close();
    const QString moved = directory.filePath(QStringLiteral("moved.yagfproj"));
    assert(QDir().rename(bundle, moved));
    assert(QFile::remove(sourcePath));
    OcrDocument relocated;
    assert(ProjectFile::loadPortable(moved, &relocated, &error));
    assert(relocated.sourcePath.startsWith(moved + QLatin1Char('/')));
    QFile copied(relocated.sourcePath);
    assert(copied.open(QIODevice::ReadOnly));
    assert(copied.readAll() == imageBytes);
    assert(relocated.page(1)->words.at(0).text == original.page(1)->words.at(0).text);
    assert(relocated.page(1)->imageRegions.first().bounds == QRect(5, 8, 95, 60));
    assert(relocated.context == original.context);
    copied.close();
    relocated.page(1)->words[0].text = QStringLiteral("portable edit");
    assert(ProjectFile::savePortable(moved, relocated, &error));
    assert(ProjectFile::loadPortable(moved, &loaded, &error));
    assert(loaded.page(1)->words.at(0).text == QStringLiteral("portable edit"));
    assert(!ProjectFile::savePortable(moved, portable, &error)); // Original source was removed.
    assert(ProjectFile::loadPortable(moved, &loaded, &error));
    assert(loaded.page(1)->words.at(0).text == QStringLiteral("portable edit"));
    assert(copied.open(QIODevice::WriteOnly | QIODevice::Truncate));
    copied.write("damaged");
    copied.close();
    assert(!ProjectFile::loadPortable(moved, &relocated, &error));

    const QString nearby = directory.filePath(QStringLiteral("nearby"));
    assert(QDir().mkpath(nearby));
    const QString nearbySource = QDir(nearby).filePath(QStringLiteral("source.png"));
    QFile localSource(nearbySource);
    assert(localSource.open(QIODevice::WriteOnly));
    localSource.write(imageBytes);
    localSource.close();
    OcrDocument nearbyDocument = original;
    nearbyDocument.sourcePath = nearbySource;
    assert(ProjectFile::save(QDir(nearby).filePath(QStringLiteral("project.yagf.json")), nearbyDocument, &error));
    const QString movedNearby = directory.filePath(QStringLiteral("nearby-moved"));
    assert(QDir().rename(nearby, movedNearby));
    assert(ProjectFile::load(QDir(movedNearby).filePath(QStringLiteral("project.yagf.json")), &loaded, &error));
    assert(loaded.sourcePath == QDir(movedNearby).filePath(QStringLiteral("source.png")));
}
