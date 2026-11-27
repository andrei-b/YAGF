// Run the document controller used by the Recognize action on each input page.
#include "documentprocessor.h"
#include <QEventLoop>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (argc < 3) {
        qWarning() << "Usage: benchmark_mixed_formulas output.json page.png ...";
        return 1;
    }
    QJsonObject results;
    for (const auto &path : app.arguments().mid(2)) {
        yagf::core::OcrDocument book;
        book.sourcePath = path;
        yagf::processing::DocumentProcessor processor(nullptr, &book);
        std::optional<yagf::core::OcrPage> page;
        QEventLoop loop;
        QObject::connect(&processor, &yagf::processing::DocumentProcessor::recognized, &loop,
                         [&](const yagf::core::OcrPage &result) { page = result; loop.quit(); });
        QObject::connect(&processor, &yagf::processing::DocumentProcessor::specialistWarning, &loop,
                         [&](const QString &message) { qWarning() << path << message; });
        bool failed = false;
        QObject::connect(&processor, &yagf::processing::DocumentProcessor::failed, &loop,
                         [&](const QString &message) { qWarning() << path << message; failed = true; loop.quit(); });
        processor.recognize(path, "eng", 1);
        if (!page && !failed) loop.exec();
        if (!page) return 1;
        qInfo() << "Recognize completed" << QFileInfo(path).completeBaseName();
        QJsonArray formulas;
        for (const auto &word : page->words) {
            if (!word.layoutBlock) continue;
            auto latex = word.text;
            if (latex.startsWith("\\(") && latex.endsWith("\\)")) latex = latex.mid(2,latex.size()-4);
            const QRect b = word.bounds;
            formulas.append(QJsonObject{{"box",QJsonArray{b.x(),b.y(),b.x()+b.width(),b.y()+b.height()}},
                                        {"latex",latex}});
        }
        const auto key = QFileInfo(path).completeBaseName();
        if (results.contains(key)) return 1;
        results.insert(key, formulas);
        if (app.arguments()[1] == "-") {
            if (path == app.arguments().last()) {
                QFile output;
                output.open(stdout, QIODevice::WriteOnly);
                output.write(QJsonDocument(results).toJson());
            }
            continue;
        }
        QSaveFile file(app.arguments()[1]);
        const auto bytes = QJsonDocument(results).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return 1;
        qInfo() << key << formulas.size();
    }
}
