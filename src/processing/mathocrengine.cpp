#include "mathocrengine.h"
#include "recognitionrecorder.h"

#include <algorithm>
#include <QMap>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QScopeGuard>

namespace yagf::processing {

namespace {
QString unwrapFormula(QString text)
{
    text = text.trimmed();
    if (text.startsWith("```")) {
        const auto newline = text.indexOf('\n');
        if (newline >= 0 && text.endsWith("```")) text = text.mid(newline+1, text.size()-newline-4).trimmed();
    }
    if ((text.startsWith("\\[") && text.endsWith("\\]")) ||
        (text.startsWith("\\(") && text.endsWith("\\)")) ||
        (text.startsWith("$$") && text.endsWith("$$"))) text = text.mid(2, text.size()-4).trimmed();
    else if (text.startsWith('$') && text.endsWith('$')) text = text.mid(1, text.size()-2).trimmed();
    return text;
}

bool validFormula(const QString &text)
{
    if (text.isEmpty() || text.size() > 65536) return false;
    int braces = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == '\\') { ++i; continue; }
        if (text[i] == '{') ++braces;
        if (text[i] == '}' && --braces < 0) return false;
    }
    return braces == 0;
}
}

QString formulaRowLatex(const core::RecognitionResult &row, const QVector<core::OcrWord> &pageWords)
{
    QMap<int, QString> parts;
    for (const auto &formula : row.formulas) {
        const QString latex = unwrapFormula(formula.latex);
        if (!validFormula(latex)) return {};
        parts.insert(formula.order, latex);
    }
    for (const auto &region : row.text) {
        QString text = region.text;
        if (region.inputArea && !pageWords.isEmpty()) {
            const QRect area = region.inputArea->translated(row.provenance.inputArea.topLeft());
            QVector<const core::OcrWord *> words;
            QRect covered;
            for (const auto &word : pageWords) {
                if (!area.contains(word.bounds.center()) || word.layoutBlock) continue;
                words.append(&word);
                covered |= word.bounds;
            }
            std::ranges::stable_sort(words, {}, [](const auto *word) { return word->bounds.left(); });
            QStringList native;
            for (const auto *word : words) native.append(word->text);
            const QString candidate = native.join(' ');
            static const QRegularExpression prose(QStringLiteral(R"(^(?=.*\p{L}.*\p{L})[\p{L}\s.,;:!?()–—-]+$)"));
            if (covered.intersected(area).width() * 4 >= area.width() * 3 && prose.match(candidate).hasMatch())
                text = candidate;
        }
        parts.insert(region.order, QStringLiteral("\\text{") + text + '}');
    }
    return parts.values().join(QStringLiteral("\\quad "));
}

std::expected<core::OcrPage, OcrError> recognizeFormulaBoxes(
    IOcrEngine *provider, const QImage &image, const OcrOptions &options, std::stop_token stop,
    const QList<QRect> &boxes, QStringList *warnings, int timeBudgetMs)
{
    if (!provider) {
        MathOcrEngine local;
        return local.recognizeBoxes(image, options, stop, boxes, warnings, timeBudgetMs);
    }
    core::OcrPage page;
    page.number = options.pageNumber;
    page.id = QStringLiteral("page-%1").arg(page.number);
    page.width = page.sourceWidth = image.width();
    page.height = page.sourceHeight = image.height();
    page.engineId = provider->id(); page.language = "auto";
    QElapsedTimer timer; timer.start();
    for (int i = 0; i < boxes.size(); ++i) {
        if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
        const auto &box = boxes[i];
        if (box.isEmpty() || !image.rect().contains(box))
            return std::unexpected(OcrError{QStringLiteral("Invalid formula crop.")});
        const int remaining = timeBudgetMs - int(timer.elapsed());
        if (remaining <= 0) {
            const QString message = QStringLiteral("Formula time budget exhausted; kept native OCR for remaining crops.");
            if (!warnings) return std::unexpected(OcrError{message});
            warnings->append(message); break;
        }
        auto settings = croppedOcrOptions(options, box);
        settings.formula = true;
        settings.historyRole = "formula";
        settings.language = "auto";
        settings.timeoutMs = std::min(remaining, 30000);
        if (options.progress) options.progress(QStringLiteral("Recognizing formulas"), i, boxes.size());
        auto reading = provider->recognize(image.copy(box), settings, stop);
        QString error, text;
        if (!reading) {
            if (reading.error().cancelled || stop.stop_requested()) return std::unexpected(reading.error());
            error = reading.error().message;
        } else {
            const auto &evidence = reading->recognitionEvidence;
            if (evidence.size() == 1 && evidence.first().structuredOutput.contains("formula_row_sections")) {
                text = formulaRowLatex(evidence.first());
            } else {
                QStringList parts;
                for (const auto &word : reading->words) parts.append(word.text);
                text = unwrapFormula(parts.join('\n'));
            }
            if (!validFormula(text))
                error = QStringLiteral("Empty or unbalanced formula reading.");
        }
        if (!error.isEmpty()) {
            if (!warnings) return std::unexpected(OcrError{error});
            warnings->append(QStringLiteral("Equation at %1,%2: %3").arg(box.x()).arg(box.y()).arg(error));
            continue;
        }
        core::OcrWord word;
        word.page = page.number; word.block = page.words.size()+1; word.line = word.paragraph = 1;
        word.bounds = box; word.layoutBlock = true; word.text = word.originalText = text;
        word.geometryInferred = true; // The selected crop is an editing extent, not a formula-model word box.
        word.readingEngine = provider->id(); word.confidence = -1;
        page.words.append(std::move(word));
        appendRecognitionEvidence(page, *reading, box.topLeft());
    }
    page.rebuildHierarchy();
    for (auto &region : page.regions) region.type = "equation";
    if (options.progress) options.progress(QStringLiteral("Recognizing formulas"), boxes.size(), boxes.size());
    return page;
}

QString MathOcrEngine::pythonPath()
{
    return QStandardPaths::locate(QStandardPaths::GenericDataLocation,
        QStringLiteral("yagf2/math-ocr/venv/bin/python"));
}

std::expected<RecognitionResult, OcrError> MathOcrEngine::recognizeResult(
    const QImage &image, const OcrOptions &options, std::stop_token stop)
{
    auto page = recognizeBoxes(image, options, stop, {});
    if (!page) return std::unexpected(page.error());
    auto result = recognitionFromPage(*page);
    result.provenance.model = "Rapid LaTeX OCR";
    result.provenance.task = "formula";
    return result;
}

std::expected<core::OcrPage, OcrError> MathOcrEngine::recognizeBoxes(
    const QImage &image, const OcrOptions &options, std::stop_token stop, const QList<QRect> &boxes,
    QStringList *warnings, int timeBudgetMs)
{
    const bool record=options.history && !options.historyNativeCall;
    core::RecognitionRun run;if(record)run=options.history->begin(id(),"Rapid LaTeX OCR",image,options);
    QElapsedTimer elapsed;elapsed.start();
    auto perform=[&]() -> std::expected<core::OcrPage,OcrError> {
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    QJsonArray requested;for(const auto &box:boxes)requested.append(QJsonArray{box.x(),box.y(),box.width(),box.height()});
    structuredOutput={{"requested_crops",requested},{"time_budget_ms",timeBudgetMs},{"crop_timeout_seconds",timeBudgetMs<=30000?5:15}};
    const QString python = pythonPath();
    if (!QFileInfo(python).isExecutable())
        return std::unexpected(OcrError{QStringLiteral("Local math OCR is not installed. See Local math OCR in the YAGF README.")});
    QTemporaryDir temporary;
    const QString input = temporary.filePath("page.png");
    const QString script = temporary.filePath("math_ocr.py");
    if (!temporary.isValid() || !image.save(input, "PNG") || !QFile::copy(":/math/math_ocr.py", script))
        return std::unexpected(OcrError{QStringLiteral("Could not prepare the local math OCR input.")});

    QProcess process;
    QStringList arguments{script, input};
    if (!boxes.isEmpty()) {
        QJsonArray crops;
        for (const auto &box : boxes)
            crops.append(QJsonArray{box.x(), box.y(), box.x() + box.width(), box.y() + box.height()});
        QFile file(temporary.filePath("boxes.json"));
        const auto data = QJsonDocument(crops).toJson(QJsonDocument::Compact);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.flush())
            return std::unexpected(OcrError{QStringLiteral("Could not prepare equation crops.")});
        arguments.append(file.fileName());
    }
    arguments << "--time-budget" << QString::number(std::max(1,timeBudgetMs)/1000.0);
    arguments << "--crop-timeout" << QString::number(timeBudgetMs<=30000 ? 5 : 15);
    QByteArray diagnostics,progressBytes;
    const auto report=[&] {
        const auto data=process.readAllStandardError(); diagnostics+=data; progressBytes+=data;
        while (progressBytes.contains('\n')) {
            const int end=progressBytes.indexOf('\n'); const auto line=progressBytes.left(end); progressBytes.remove(0,end+1);
            if (!line.startsWith("YAGF_PROGRESS ")) continue;
            const auto value=QJsonDocument::fromJson(line.mid(14)).object();
            if (options.progress && value["total"].toInt()>0) options.progress(QStringLiteral("Recognizing formulas"),value["done"].toInt(),value["total"].toInt());
        }
        if (!options.history && diagnostics.size()>16384) diagnostics=diagnostics.right(16384);
        if (progressBytes.size()>16384) progressBytes=progressBytes.right(16384);
    };
    const auto capture=qScopeGuard([&] {
        report();structuredOutput["stderr"]=QString::fromUtf8(diagnostics);
        if(!structuredOutput.contains("results") && !structuredOutput.contains("raw_output_base64"))
            structuredOutput["raw_output_base64"]=QString::fromLatin1(process.readAllStandardOutput().toBase64());
    });
    if (options.progress) options.progress(QStringLiteral("Loading formula recognizer"),0,boxes.size());
    process.start(python, arguments);
    if (!process.waitForStarted())
        return std::unexpected(OcrError{process.errorString()});
    QElapsedTimer timer;
    timer.start();
    while (!process.waitForFinished(100) && process.state() != QProcess::NotRunning) {
        report();
        if (stop.stop_requested() || timer.elapsed() > timeBudgetMs+10000) {
            process.kill();
            process.waitForFinished();
            return std::unexpected(OcrError{QStringLiteral("Local math OCR timed out."), stop.stop_requested()});
        }
    }
    report();
    structuredOutput["stderr"]=QString::fromUtf8(diagnostics);
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return std::unexpected(OcrError{QStringLiteral("Local math OCR failed: %1")
            .arg(QString::fromUtf8(diagnostics).trimmed().section('\n', -1))});
    structuredOutput["stderr"]=QString::fromUtf8(diagnostics);
    const auto bytes=process.readAllStandardOutput();
    const auto json = QJsonDocument::fromJson(bytes);
    structuredOutput["stderr"]=QString::fromUtf8(diagnostics);
    if(json.isArray())structuredOutput["results"]=json.array();
    else structuredOutput["raw_output_base64"]=QString::fromLatin1(bytes.toBase64());
    if (!json.isArray()) return std::unexpected(OcrError{QStringLiteral("Invalid local math OCR output.")});

    core::OcrPage page;
    page.number = options.pageNumber;
    page.id = QStringLiteral("page-%1").arg(page.number);
    page.width = page.sourceWidth = image.width();
    page.height = page.sourceHeight = image.height();
    page.language = QStringLiteral("auto");
    page.engineId = id();
    QStringList failures;
    int deferred=0;
    QList<QRect> seen;
    for (const auto &item : json.array()) {
        const auto entry = item.toObject();
        const auto box = entry.value("box").toArray();
        const QString text = entry.value("text").toString().trimmed();
        if (box.size() != 4)
            return std::unexpected(OcrError{QStringLiteral("Invalid local math OCR crop.")});
        const QRect bounds(box.at(0).toInt(-1), box.at(1).toInt(-1),
                           box.at(2).toInt(), box.at(3).toInt());
        if (bounds.isEmpty() || !image.rect().contains(bounds) || seen.contains(bounds) ||
            (!boxes.isEmpty() && !boxes.contains(bounds)))
            return std::unexpected(OcrError{QStringLiteral("Invalid local math OCR reading.")});
        seen.append(bounds);
        if (!entry.value("error").toString().isEmpty()) {
            if (entry["error"].toString()=="Formula time budget exhausted") {++deferred; continue;}
            failures.append(QStringLiteral("Equation at %1,%2: %3")
                .arg(bounds.x()).arg(bounds.y()).arg(entry.value("error").toString()));
            continue;
        }
        if (text.isEmpty()) return std::unexpected(OcrError{QStringLiteral("Empty local math OCR reading.")});
        core::OcrWord word;
        word.page = page.number;
        word.block = page.words.size() + 1;
        word.line = word.paragraph = 1;
        word.layoutBlock = true;
        word.bounds = bounds;
        word.text = word.originalText = text;
        word.readingEngine = id();
        page.words.append(std::move(word));
    }
    if (!boxes.isEmpty() && seen.size() != boxes.size())
        return std::unexpected(OcrError{QStringLiteral("Local math OCR returned an incomplete set of crops.")});
    if (deferred) failures.append(QStringLiteral("Kept native OCR for %1 formula candidate(s) after the formula time budget was reached. Select a smaller block to retry.").arg(deferred));
    if (!failures.isEmpty()) {
        if (!warnings) return std::unexpected(OcrError{failures.join('\n')});
        *warnings += failures;
    }
    page.rebuildHierarchy();
    for (auto &region : page.regions) region.type = QStringLiteral("equation");
    auto evidence = recognitionFromPage(page);
    evidence.structuredOutput=structuredOutput;
    evidence.provenance.model = "Rapid LaTeX OCR"; evidence.provenance.task = "formula";
    page.recognitionEvidence.append(std::move(evidence));
    return page;
    };
    auto result=perform();
    if(record) {
        QJsonArray crops;for(const auto &box:boxes)crops.append(QJsonArray{box.x(),box.y(),box.width(),box.height()});
        auto output=structuredOutput;output["requested_crops"]=crops;
        auto evidence=options.history->finish(std::move(run),elapsed.elapsed(),result?"succeeded":result.error().cancelled?"cancelled":"failed",
            result?QString():result.error().message,output,{},warnings?*warnings:QStringList{});
        if(result)result->recognitionHistory.runs.append(std::move(evidence));
    }
    return result;
}

} // namespace yagf::processing
