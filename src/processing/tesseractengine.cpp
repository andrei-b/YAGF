#include "tesseractengine.h"
#include "recognitionrecorder.h"
#include <QScopeGuard>
#include <QXmlStreamReader>
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

namespace {
bool supportsInlineScripts(const QString &executable, std::stop_token stop,
                           const std::atomic_bool *cancelled)
{
    static std::mutex mutex;
    static QHash<QString, bool> cache;
    const QFileInfo file(executable);
    const QString key = executable + ':' + QString::number(file.lastModified().toMSecsSinceEpoch());
    {
        std::lock_guard guard(mutex);
        if (cache.contains(key)) return cache.value(key);
    }
    QProcess probe;
    probe.start(executable, {"--print-parameters"});
    QElapsedTimer timer;
    timer.start();
    while (!probe.waitForFinished(50) && probe.state() != QProcess::NotRunning) {
        if (stop.stop_requested() || (cancelled && cancelled->load()) || timer.elapsed() >= 2000) {
            probe.kill();
            probe.waitForFinished();
            return false;
        }
    }
    const bool available = probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0 &&
        probe.readAllStandardOutput().contains("\nlstm_reread_subscripts\t");
    std::lock_guard guard(mutex);
    cache.insert(key, available);
    return available;
}

QRect hocrBox(QStringView title, QStringView name)
{
    if (!title.startsWith(name)) return {};
    const auto values = title.sliced(name.size()).toString().section(';', 0, 0).split(' ', Qt::SkipEmptyParts);
    if (values.size() != 4) return {};
    int edges[4];
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        edges[i] = values[i].toInt(&ok);
        if (!ok || edges[i] < 0) return {};
    }
    return {edges[0], edges[1], edges[2] - edges[0], edges[3] - edges[1]};
}

// Join by bounds and full transcription. Both renderers use the same recognition run.
bool applyHocrScripts(core::OcrPage &page, QIODevice &input)
{
    QMultiHash<QPoint, int> byOrigin;
    for (int i = 0; i < page.words.size(); ++i) byOrigin.insert(page.words[i].bounds.topLeft(), i);
    QXmlStreamReader xml(&input);
    int depth = 0, wordDepth = -1, glyphDepth = -1;
    QRect wordBox, glyphBox;
    QString text, glyphText;
    std::optional<core::ScriptPosition> position;
    QVector<core::ScriptGlyph> glyphs;
    QHash<int, QVector<core::ScriptGlyph>> found;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            ++depth;
            const auto attrs = xml.attributes();
            const auto cls = attrs.value("class");
            if (cls == u"ocrx_word") {
                wordDepth = depth;
                wordBox = hocrBox(attrs.value("title"), u"bbox ");
                text.clear();
                glyphs.clear();
            } else if (wordDepth >= 0 && cls == u"ocrx_cinfo" &&
                       attrs.value("title").startsWith(u"x_bboxes ")) {
                glyphDepth = depth;
                glyphBox = hocrBox(attrs.value("title"), u"x_bboxes ");
                glyphText.clear();
                position.reset();
            } else if (glyphDepth >= 0 && xml.name() == u"sub") {
                position = core::ScriptPosition::Subscript;
            } else if (glyphDepth >= 0 && xml.name() == u"sup") {
                position = core::ScriptPosition::Superscript;
            }
        } else if (xml.isCharacters() && glyphDepth >= 0) {
            glyphText.append(xml.text());
        } else if (xml.isEndElement()) {
            if (depth == glyphDepth) {
                if (position && !glyphText.isEmpty() && !glyphBox.isEmpty() && wordBox.contains(glyphBox)) {
                    const qreal w = wordBox.width(), h = wordBox.height();
                    glyphs.append({int(text.size()), int(glyphText.size()),
                                   {(glyphBox.x()-wordBox.x())/w, (glyphBox.y()-wordBox.y())/h,
                                    glyphBox.width()/w, glyphBox.height()/h}, *position});
                }
                text += glyphText;
                glyphDepth = -1;
            }
            if (depth == wordDepth) {
                if (!glyphs.isEmpty()) {
                    for (int i : byOrigin.values(wordBox.topLeft())) {
                        if (page.words[i].bounds == wordBox && page.words[i].originalText == text) {
                            found.insert(i, glyphs);
                            break;
                        }
                    }
                }
                wordDepth = -1;
            }
            --depth;
        }
    }
    if (xml.hasError()) return false;
    for (auto it = found.cbegin(); it != found.cend(); ++it) page.words[it.key()].scripts = it.value();
    return true;
}
} // namespace

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
    const auto executable = QStandardPaths::findExecutable(QStringLiteral("tesseract"));
    if (executable.isEmpty())
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
    const bool scripts = supportsInlineScripts(executable, stop, cancelled);
    if (interrupted()) return std::unexpected(OcrError{{}, true});
    const QString outputBase = temporary.filePath("recognition");
    if (scripts) {
        arguments[1] = outputBase;
        arguments << "-c" << "lstm_detect_subscripts=1" << "-c" << "lstm_recover_subscripts=1"
                  << "-c" << "lstm_reread_subscripts=1" << "-c" << "hocr_char_boxes=1"
                  << "-c" << "tessedit_create_tsv=1" << "-c" << "tessedit_create_hocr=1";
    } else {
        arguments << QStringLiteral("tsv");
    }
    QProcess process;
    const auto capture=qScopeGuard([&]{
        if(!structuredOutput.contains("tsv")) {
            structuredOutput["format"]="tesseract-tsv";
            structuredOutput["tsv"]=QString::fromUtf8(process.readAllStandardOutput());
            structuredOutput["stderr"]=QString::fromUtf8(process.readAllStandardError());
            structuredOutput["exit_code"]=process.exitCode();
        }
    });
    QJsonObject modelEvidence{{"inline_subscripts", scripts}};
    if(options.history) {
        // Resolve version and data location once per executable/environment.
        // Recheck traineddata fingerprints independently, using file identity.
        static std::mutex metadataMutex;static QHash<QString,QPair<QString,QString>> metadataCache;
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
        QStringList measured{"-f","%M","-o",memoryFile,"--",executable};
        measured+=arguments;process.start(QStringLiteral("/usr/bin/time"),measured);
    } else process.start(executable, arguments);
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
    QByteArray tsv = process.readAllStandardOutput();
    if (scripts) {
        QFile file(outputBase + ".tsv");
        if (file.open(QIODevice::ReadOnly)) tsv = file.readAll();
    }
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
    auto page = parseTsv(tsv, image.size(), resolved);
    if (scripts && page) {
        QFile hocr(outputBase + ".hocr");
        if (!hocr.open(QIODevice::ReadOnly) || !applyHocrScripts(*page, hocr))
            structuredOutput["warnings"] = QJsonArray{QStringLiteral("Tesseract's hOCR output could not be read for script positions.")};
    }
    return page;
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
