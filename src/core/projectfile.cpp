#include "projectfile.h"
#include "coordinateframes.h"

#include <QFile>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <unistd.h>

namespace yagf::core {

namespace {
QString portableManifest(const QString &directory)
{
    return QDir(directory).filePath(QStringLiteral("project.yagf.json"));
}

QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return hash.result().toHex();
}

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}
}

bool ProjectFile::save(const QString &path, const OcrDocument &document, QString *error)
{
    for (const auto &page:document.pages) {
        auto valid=validateRecognitionHistory(page.recognitionHistory);
        if(!valid) { if(error)*error=valid.error(); return false; }
    }
    const QString temporaryPath = path + QLatin1Char('.') +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".tmp");
    QFile file(temporaryPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    OcrDocument stored = document;
    if (QDir::isAbsolutePath(stored.sourcePath)) {
        const QString relative = QDir(QFileInfo(path).absolutePath()).relativeFilePath(stored.sourcePath);
        if (!relative.startsWith(QStringLiteral("../")) && relative != QStringLiteral("..") &&
            !QDir::isAbsolutePath(relative)) stored.sourcePath = relative;
    }
    const auto documentJson=stored.toJson();
    for(const auto &page:documentJson["pages"].toArray()) {
        CoordinateFrames frames;
        if(auto valid=frames.load(page.toObject()["recognition_frames"].toObject());!valid) {
            if(error)*error=valid.error();file.close();QFile::remove(temporaryPath);return false;
        }
    }
    const QJsonObject project{{"project_version", 2}, {"document", documentJson}};
    const QByteArray bytes = QJsonDocument(project).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.flush() || ::fsync(file.handle()) != 0) {
        if (error) *error = file.error() == QFileDevice::NoError
            ? QString::fromLocal8Bit(std::strerror(errno)) : file.errorString();
        file.close();
        QFile::remove(temporaryPath);
        return false;
    }
    const QByteArray temporaryName = QFile::encodeName(temporaryPath);
    const QByteArray targetName = QFile::encodeName(path);
    file.close();
    if (::rename(temporaryName.constData(), targetName.constData()) != 0) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        QFile::remove(temporaryPath);
        return false;
    }
    return true;
}

bool ProjectFile::load(const QString &path, OcrDocument *document, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        if (error) *error = QStringLiteral("Invalid project JSON: %1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = json.object();
    if (!root.value(QStringLiteral("project_version")).isDouble() ||
        (root.value(QStringLiteral("project_version")).toInt() != 1 &&
         root.value(QStringLiteral("project_version")).toInt() != 2) ||
        !root.value(QStringLiteral("document")).isObject()) {
        if (error) *error = QStringLiteral("Unsupported project version or missing document.");
        return false;
    }
    OcrDocument parsed;
    if (!OcrDocument::fromJson(root.value(QStringLiteral("document")).toObject(), &parsed, error))
        return false;
    if (QDir::isRelativePath(parsed.sourcePath))
        parsed.sourcePath = QDir(QFileInfo(path).absolutePath()).absoluteFilePath(parsed.sourcePath);
    if (!document) return fail(error, QStringLiteral("No output document."));
    *document = std::move(parsed);
    return true;
}

bool ProjectFile::savePortable(const QString &directory, const OcrDocument &document, QString *error)
{
    if (!QFileInfo(document.sourcePath).isFile())
        return fail(error, QStringLiteral("The source document is missing: %1").arg(document.sourcePath));
    const QFileInfo bundleInfo(directory);
    if (bundleInfo.exists() && !bundleInfo.isDir())
        return fail(error, QStringLiteral("A file already exists at the portable project path."));
    QDir bundle(directory);
    if (!bundle.exists() && !QDir().mkpath(directory))
        return fail(error, QStringLiteral("Could not create the portable project directory."));
    if (!bundle.mkpath(QStringLiteral("assets")))
        return fail(error, QStringLiteral("Could not create the project assets directory."));
    const QByteArray digest = fileHash(document.sourcePath);
    if (digest.isEmpty()) return fail(error, QStringLiteral("Could not read the source document."));
    QString suffix = QFileInfo(document.sourcePath).suffix().toLower();
    if (!QRegularExpression(QStringLiteral("^[a-z0-9]{1,12}$")).match(suffix).hasMatch())
        suffix = QStringLiteral("bin");
    const QString assetName = QString::fromLatin1(digest) + QLatin1Char('.') + suffix;
    const QString assetPath = bundle.filePath(QStringLiteral("assets/") + assetName);
    if (fileHash(assetPath) != digest) {
        QFile input(document.sourcePath);
        QSaveFile output(assetPath);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
            return fail(error, QStringLiteral("Could not copy the source document into the project."));
        while (!input.atEnd()) {
            const QByteArray chunk = input.read(1024 * 1024);
            if (chunk.isEmpty() || output.write(chunk) != chunk.size()) {
                output.cancelWriting();
                return fail(error, QStringLiteral("Could not write the project source asset."));
            }
        }
        if (!output.commit())
            return fail(error, QStringLiteral("Could not complete the project source asset."));
        if (fileHash(assetPath) != digest) {
            QFile::remove(assetPath);
            return fail(error, QStringLiteral("The source changed while the portable project was being saved."));
        }
    }
    OcrDocument portable = document;
    portable.sourcePath = QFileInfo(assetPath).absoluteFilePath();
    return save(portableManifest(directory), portable, error);
}

bool ProjectFile::loadPortable(const QString &directory, OcrDocument *document, QString *error)
{
    if (!QFileInfo(directory).isDir())
        return fail(error, QStringLiteral("Portable project directory is missing."));
    OcrDocument loaded;
    if (!load(portableManifest(directory), &loaded, error)) return false;
    const QString assets = QFileInfo(QDir(directory).filePath(QStringLiteral("assets"))).canonicalFilePath();
    const QString source = QFileInfo(loaded.sourcePath).canonicalFilePath();
    if (assets.isEmpty() || source.isEmpty() ||
        !source.startsWith(assets + QLatin1Char('/')))
        return fail(error, QStringLiteral("The portable project source is outside its assets directory."));
    const QByteArray expected = QFileInfo(source).completeBaseName().toLatin1();
    if (expected.size() != 64 || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
            .match(QString::fromLatin1(expected)).hasMatch() || fileHash(source) != expected)
        return fail(error, QStringLiteral("The portable project source failed its integrity check."));
    if (!document) return fail(error, QStringLiteral("No output document."));
    *document = std::move(loaded);
    return true;
}

} // namespace yagf::core
