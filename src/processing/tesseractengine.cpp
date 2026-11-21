#include "tesseractengine.h"
#include "recognitionrecorder.h"
#include <QScopeGuard>
#include <QJsonDocument>
#include <QDir>
#include <QJsonArray>
#include <QRegularExpression>

#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace yagf::processing {

std::expected<TesseractLanguages,QString> installedTesseractLanguages()
{
    QProcess process;
    process.start("tesseract", {"--list-langs"});
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const auto error = process.error() == QProcess::UnknownError
            ? QString::fromUtf8(process.readAllStandardError()).trimmed() : process.errorString();
        if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(); }
        return std::unexpected(error);
    }
    const QString output = QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
    static const QRegularExpression location(QStringLiteral("List of available languages in \"([^\"]+)\""));
    static const QRegularExpression code(QStringLiteral("^[A-Za-z][A-Za-z0-9_/-]*$"));
    TesseractLanguages languages;
    languages.directory = location.match(output).captured(1);
    for (const auto &line : output.split('\n')) {
        const auto name = line.trimmed();
        if (name != "osd" && code.match(name).hasMatch()) languages.codes.append(name);
    }
    languages.codes.removeDuplicates();
    languages.codes.sort(Qt::CaseInsensitive);
    return languages;
}

QString TesseractEngine::modelLanguage(const QString &language)
{
    // Region settings and imported projects can carry the short provider codes.
    const auto selected = language.trimmed();
    if (selected.isEmpty() || selected == QStringLiteral("auto")) return QStringLiteral("eng");
    QStringList languages = selected.split('+');
    for (QString &code : languages) {
        code = code.trimmed();
        if (code == QStringLiteral("en")) code = QStringLiteral("eng");
        else if (code == QStringLiteral("ru")) code = QStringLiteral("rus");
        else if (code == QStringLiteral("de")) code = QStringLiteral("deu");
    }
    languages.removeDuplicates();
    return languages.join('+');
}

std::expected<RecognitionResult, OcrError> TesseractEngine::recognizeResult(
    const QImage &image, const OcrOptions &options, std::stop_token stop)
{
    auto page = recognize(image, options, stop, nullptr);
    if (!page) return std::unexpected(page.error());
    auto result = recognitionFromPage(*page);
    result.provenance.model = page->language;
    result.provenance.task = "ocr";
    result.structuredOutput=structuredOutput;
    result.modelVersion=structuredOutput["model_version"].toString();
    return result;
}

std::expected<core::OcrPage, OcrError> TesseractEngine::recognize(
    const QImage &image, const OcrOptions &options, std::stop_token stop, const std::atomic_bool *cancelled)
{
    runDetails = {}; structuredOutput={};
    const auto interrupted = [&] { return stop.stop_requested() || (cancelled && cancelled->load()); };
    if (interrupted()) return std::unexpected(OcrError{{}, true});
    if (image.isNull() || options.pageNumber < 1)
        return std::unexpected(OcrError{QStringLiteral("Invalid OCR image or options.")});
    if (QStandardPaths::findExecutable(QStringLiteral("tesseract")).isEmpty())
        return std::unexpected(OcrError{QStringLiteral("tesseract is not installed or is not on PATH.")});
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return std::unexpected(OcrError{QStringLiteral("Could not create OCR temporary files.")});
    const QString imagePath = temporary.filePath(QStringLiteral("input.png"));
    if (!image.save(imagePath, "PNG"))
        return std::unexpected(OcrError{QStringLiteral("Could not prepare the OCR image.")});
    if (interrupted()) return std::unexpected(OcrError{{}, true});
    OcrOptions resolved = options;
    if (options.progress) options.progress(QStringLiteral("Reading text with Tesseract"),0,0);
    resolved.language = modelLanguage(options.language);
    QStringList arguments{imagePath, QStringLiteral("stdout"), QStringLiteral("-l"), resolved.language};
    if (options.singleTextLine) arguments << QStringLiteral("--psm") << QStringLiteral("7");
    if (!options.userWords.isEmpty()) {
        QFile vocabulary(temporary.filePath(QStringLiteral("book-words.txt")));
        const QByteArray data = options.userWords.join('\n').toUtf8() + '\n';
        if (!vocabulary.open(QIODevice::WriteOnly) || vocabulary.write(data) != data.size() || !vocabulary.flush())
            return std::unexpected(OcrError{QStringLiteral("Could not prepare the book vocabulary.")});
        arguments << QStringLiteral("--user-words") << vocabulary.fileName();
    }
    arguments << QStringLiteral("tsv");
    QProcess process;
    const auto capture=qScopeGuard([&]{
        if(!structuredOutput.contains("tsv")) {
            structuredOutput["format"]="tesseract-tsv";
            structuredOutput["tsv"]=QString::fromUtf8(process.readAllStandardOutput());
            structuredOutput["stderr"]=QString::fromUtf8(process.readAllStandardError());
            structuredOutput["exit_code"]=process.exitCode();
        }
    });
    QJsonObject modelEvidence;
    if(options.history) {
        // Resolve version and data location once per executable/environment.
        // Recheck traineddata fingerprints independently, using file identity.
        static std::mutex metadataMutex;static QHash<QString,QPair<QString,QString>> metadataCache;
        const auto executable=QStandardPaths::findExecutable("tesseract");
        const auto key=executable+':'+QString::number(QFileInfo(executable).lastModified().toMSecsSinceEpoch())+':'+qEnvironmentVariable("TESSDATA_PREFIX");
        QPair<QString,QString> metadataValue;
        {
            std::lock_guard lock(metadataMutex);
            if(!metadataCache.contains(key)) {
                QPair<QString,QString> found;QProcess metadata;metadata.start(executable,{"--version"});
                if(metadata.waitForFinished(2000))found.first=QString::fromUtf8(metadata.readAllStandardOutput()).section('\n',0,0).trimmed();
                if(metadata.state()!=QProcess::NotRunning){metadata.kill();metadata.waitForFinished();}
                metadata.start(executable,{"--list-langs"});
                if(metadata.waitForFinished(2000)) {
                    const auto message=QString::fromUtf8(metadata.readAllStandardOutput()+metadata.readAllStandardError());
                    const auto location=QRegularExpression(QStringLiteral("List of available languages in \"([^\"]+)\"")).match(message);
                    if(location.hasMatch())found.second=location.captured(1);
                }
                if(metadata.state()!=QProcess::NotRunning){metadata.kill();metadata.waitForFinished();}
                if(metadataCache.size()>32)metadataCache.clear();metadataCache.insert(key,found);
            }
            metadataValue=metadataCache[key];
        }
        modelEvidence["model_version"]=metadataValue.first;
        if(!metadataValue.second.isEmpty()) {
            {
                QJsonArray models;bool complete=true;
                for(const auto &language:resolved.language.split('+')) {
                    const auto hash=recognitionSourceHash(QDir(metadataValue.second).filePath(language+".traineddata"),stop);
                    if(hash.isEmpty()){complete=false;break;}
                    models.append(QJsonObject{{"language",language},{"sha256",hash}});
                }
                if(complete && !models.isEmpty()) {
                    modelEvidence["traineddata"]=models;
                    modelEvidence["model_hash"]=models.size()==1?models.first().toObject()["sha256"]:QJsonValue(core::sha256(QJsonDocument(models).toJson(QJsonDocument::Compact)));
                    modelEvidence["model_hash_kind"]=models.size()==1?"traineddata-sha256":"traineddata-manifest-sha256";
                }
            }
        }
    }
    structuredOutput=modelEvidence;
    auto environment = QProcessEnvironment::systemEnvironment();
    // Avoid excessive OpenMP workers on document/region queues; honor user overrides.
    if (!environment.contains("OMP_THREAD_LIMIT")) environment.insert("OMP_THREAD_LIMIT", "2");
    process.setProcessEnvironment(environment);
    runDetails["omp_thread_limit"]=environment.value("OMP_THREAD_LIMIT");
    const QString memoryFile = temporary.filePath(QStringLiteral("peak-rss.txt"));
    const bool measureMemory=options.collectBenchmarkMetrics && QFile::exists(QStringLiteral("/usr/bin/time"));
    if(measureMemory) {
#ifdef Q_OS_UNIX
        // Cancel GNU time and its recognition child together.
        process.setChildProcessModifier([]{::setsid();});
#endif
        QStringList measured{"-f","%M","-o",memoryFile,"--",QStandardPaths::findExecutable(QStringLiteral("tesseract"))};
        measured+=arguments;process.start(QStringLiteral("/usr/bin/time"),measured);
    } else process.start(QStringLiteral("tesseract"), arguments);
    if (!process.waitForStarted(10'000))
        return std::unexpected(OcrError{QStringLiteral("Could not start tesseract: %1")
            .arg(process.errorString())});
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning) {
        if (interrupted() || timer.elapsed() > 300'000) {
#ifdef Q_OS_UNIX
            if(measureMemory && process.processId()>0)::kill(-process.processId(),SIGKILL);
#endif
            process.kill();
            process.waitForFinished(2000);
            if (interrupted()) return std::unexpected(OcrError{{}, true});
            return std::unexpected(OcrError{QStringLiteral("Tesseract timed out.")});
        }
        process.waitForFinished(100);
    }
    const auto tsv=process.readAllStandardOutput();
    const auto diagnostics=process.readAllStandardError();
    structuredOutput["format"]="tesseract-tsv";structuredOutput["tsv"]=QString::fromUtf8(tsv);
    structuredOutput["stderr"]=QString::fromUtf8(diagnostics);structuredOutput["exit_code"]=process.exitCode();
    if (interrupted()) return std::unexpected(OcrError{{}, true});
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return std::unexpected(OcrError{QStringLiteral("Tesseract failed: %1")
            .arg(QString::fromUtf8(diagnostics).trimmed())});
    if(measureMemory) {
        QFile memory(memoryFile);
        if(memory.open(QIODevice::ReadOnly)) {
            bool ok=false;const auto kib=memory.readAll().trimmed().toLongLong(&ok);
            if(ok && kib>0){runDetails["peak_rss_bytes"]=kib*1024;runDetails["peak_rss_scope"]="Recognition child process";}
        }
    }
    return parseTsv(tsv, image.size(), resolved);
}

std::expected<core::OcrPage, OcrError> TesseractEngine::parseTsv(
    const QByteArray &tsv, const QSize &imageSize, const OcrOptions &options)
{
    const QStringList rows = QString::fromUtf8(tsv).split(QLatin1Char('\n'));
    if (rows.isEmpty() || !rows.first().startsWith(QStringLiteral("level\t")))
        return std::unexpected(OcrError{QStringLiteral("Tesseract returned invalid TSV output.")});
    core::OcrPage page;
    page.id = QStringLiteral("page-%1").arg(options.pageNumber);
    page.number = options.pageNumber;
    page.width = page.sourceWidth = imageSize.width();
    page.height = page.sourceHeight = imageSize.height();
    page.engineId = QStringLiteral("tesseract");
    page.language = options.language;
    for (int i = 1; i < rows.size(); ++i) {
        const QStringList fields = rows.at(i).split(QLatin1Char('\t'));
        if (fields.size() < 12 || fields.at(0) != QStringLiteral("5")) continue;
        bool ok[8] = {};
        core::OcrWord word;
        word.page = options.pageNumber;
        word.block = fields.at(2).toInt(&ok[0]);
        word.paragraph = fields.at(3).toInt(&ok[1]);
        word.line = fields.at(4).toInt(&ok[2]);
        const int x = fields.at(6).toInt(&ok[3]);
        const int y = fields.at(7).toInt(&ok[4]);
        const int width = fields.at(8).toInt(&ok[5]);
        const int height = fields.at(9).toInt(&ok[6]);
        word.confidence = fields.at(10).toDouble(&ok[7]);
        word.originalText = fields.mid(11).join(QLatin1Char('\t')).trimmed();
        word.text = word.originalText;
        bool valid = x >= 0 && y >= 0 && width > 0 && height > 0 &&
            static_cast<qint64>(x) + width <= imageSize.width() &&
            static_cast<qint64>(y) + height <= imageSize.height();
        for (bool value : ok) valid = valid && value;
        if (!valid) continue;
        word.bounds = QRect(x, y, width, height);
        if (word.text.isEmpty()) {
            // Tesseract represents image blocks, including unread display
            // equations, as word rows whose transcription is whitespace.
            if (word.confidence >= 0)
                page.imageRegions.append({QStringLiteral("%1/image-%2").arg(page.id).arg(i),
                    word.bounds, {}, false, {}, true});
            continue;
        }
        page.words.append(std::move(word));
    }
    page.rebuildHierarchy();
    return page;
}

} // namespace yagf::processing
