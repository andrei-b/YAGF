#include "projectfile.h"

#include <QFile>
#include <QJsonDocument>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <unistd.h>

bool ProjectFile::save(const QString &path, const OcrDocument &document, QString *error)
{
    const QString temporaryPath = path + QLatin1Char('.') +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".tmp");
    QFile file(temporaryPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    const QJsonObject project{{"project_version", 2}, {"document", document.toJson()}};
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
    return OcrDocument::fromJson(root.value(QStringLiteral("document")).toObject(), document, error);
}
