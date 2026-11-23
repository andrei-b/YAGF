#include "layoutcorrection.h"
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
    {
        OcrPage page;
        page.width = 64;
        page.height = 48;
        QImage scan(page.width, page.height, QImage::Format_RGB32);
        scan.fill(Qt::white);
        scan.setPixelColor(12, 15, Qt::black);
        QVector<ImageRegion> original{{"automatic", {10, 10, 32, 20}, {}, false, "text", true}};
        auto revised = original;
        revised[0].content = "math";
        revised[0].setShape(QRegion(QRect(10,10,32,10)) | QRect(10,20,24,10));
        beginLayoutSplit(page);
        assert(recordLayoutCorrection(page, scan, original, original, "ai-page"));
        assert(page.layoutTraining.examples.isEmpty());
        assert(recordLayoutCorrection(page, scan, original, revised, "block-edit"));
        auto stored = readLayoutTraining(layoutTrainingJson(page.layoutTraining));
        assert(stored && stored->examples.size() == 1);
        page.layoutTraining = *stored;
        assert(QImage::fromData(stored->examples[0].png).pixelColor(12,15) == Qt::black);
        assert(stored->examples[0].corrected[0].toObject()["parts"].toArray().size() == 2);
        auto moved = revised;
        moved[0].bounds.translate(2, 4);
        assert(recordLayoutCorrection(page, scan, revised, moved, "block-edit"));
        assert(page.layoutTraining.examples.size() == 1);
        assert(page.layoutTraining.examples[0].proposed == layoutAnnotations(original));
        assert(page.layoutTraining.examples[0].corrected == layoutAnnotations(moved));
        beginLayoutSplit(page);
        assert(recordLayoutCorrection(page, scan, moved, revised, "block-edit"));
        assert(page.layoutTraining.examples.size() == 2);
        assert(layoutTrainingJson(page.layoutTraining)["images"].toObject().size() == 1);
        assert(recordLayoutCorrection(page, scan, revised, moved, "block-edit"));
        assert(page.layoutTraining.examples.size() == 1);
    }
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
    original.page(1)->preprocessing = {.deskewDegrees = 1.5, .autoContrast = true, .denoise = false, .quarterTurns = 3, .adaptiveBinarization = true, .inkThreshold = 31, .bendPercent = -3.5, .levelShadows = true};
    original.page(1)->sourceWidth = 198;
    original.page(1)->sourceHeight = 98;
    original.page(1)->imageRegions = {{QStringLiteral("selected-area"), QRect(5, 8, 95, 60)}};
    original.page(1)->imageRegions[0].language = QStringLiteral("deu");
    original.page(1)->imageRegions[0].notText = true;
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
    assert(loaded.page(1)->preprocessing.quarterTurns==3 && loaded.page(1)->preprocessing.adaptiveBinarization);
    assert(loaded.page(1)->preprocessing == original.page(1)->preprocessing);
    assert(loaded.page(1)->sourceWidth == 198);
    assert(loaded.page(1)->imageRegions.size() == 1);
    assert(loaded.page(1)->imageRegions.first().bounds == QRect(5, 8, 95, 60));
    assert(loaded.page(1)->imageRegions.first().language == "deu");
    assert(loaded.page(1)->imageRegions.first().notText);
    assert(loaded.totalPages == 2);
    assert(loaded.page(2)->words.at(0).text == QStringLiteral("Hallo!"));
    assert(loaded.page(2)->regions.at(0).paragraphs.at(0).lines.at(0).wordIds.size() == 1);

    {
        auto sequence = original;
        sequence.materializePageSources();
        sequence.pageSources[1] = {directory.filePath("second.png"), 1};
        assert(ProjectFile::save(path, sequence, &error));
        assert(ProjectFile::load(path, &loaded, &error));
        assert(loaded.sourceForPage(2).path == sequence.pageSources[1].path);
        assert(loaded.sourceForPage(2).page == 1);
        auto json = sequence.toJson();
        json["page_sources"] = QJsonArray{QJsonObject{{"path", "image.png"}, {"page", 1}}};
        assert(!OcrDocument::fromJson(json, &loaded, &error));
        json["page_sources"] = QJsonArray{QJsonObject{{"path", "image.png"}, {"page", 0}},
                                         QJsonObject{{"path", "second.png"}, {"page", 1}}};
        assert(!OcrDocument::fromJson(json, &loaded, &error));
    }

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
    assert(loaded.page(1)->preprocessing.bendPercent == 0 && !loaded.page(1)->preprocessing.levelShadows);
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
    assert(relocated.page(1)->imageRegions.first().language == "deu");
    assert(relocated.page(1)->imageRegions.first().notText);
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

    // Saving keeps the file's permissions and writes through a symlink instead of replacing it.
    const QString linked = QDir(movedNearby).filePath(QStringLiteral("linked.yagf.json"));
    assert(ProjectFile::save(path, original, &error));
    const auto restricted = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    assert(QFile::setPermissions(path, restricted));
    assert(QFile::link(path, linked));
    OcrDocument edited = original;
    edited.sourcePath = QDir(movedNearby).filePath(QStringLiteral("source.png"));
    edited.page(1)->words[0].text = QStringLiteral("through link");
    assert(ProjectFile::save(linked, edited, &error));
    assert(QFileInfo(linked).isSymLink());
    const auto modeBits = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
        QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
        QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    assert((QFile::permissions(path) & modeBits) == restricted);
    assert(ProjectFile::load(path, &loaded, &error));
    assert(loaded.sourcePath == edited.sourcePath);
    assert(loaded.page(1)->words.at(0).text == QStringLiteral("through link"));
    assert(ProjectFile::load(linked, &loaded, &error));
    assert(loaded.sourcePath == edited.sourcePath);
    assert(QDir(directory.path()).entryList({QStringLiteral("*.tmp")}, QDir::Files).isEmpty());
}
