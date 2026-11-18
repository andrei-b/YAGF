#include "ui/mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QIcon>
#include <QUrl>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setDesktopFileName(QStringLiteral("io.github.andrei_b.YAGF"));
    QIcon icon(QStringLiteral(":/legacy/yagf-48.png"));
    icon.addFile(QStringLiteral(":/legacy/yagf.png"));
    QApplication::setWindowIcon(icon);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("YAGF OCR studio"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("paddleocr-vl-url"),
        QStringLiteral("Use a PaddleOCR-VL YAGF bridge as the page OCR backend."), QStringLiteral("url")});
    parser.addPositionalArgument(QStringLiteral("document"), QStringLiteral("Image, PDF, DjVu or YAGF project to open."),
                                 QStringLiteral("[document]"));
    parser.process(app);
    if (parser.positionalArguments().size() > 1) parser.showHelp(2);

    yagf::ui::MainWindow window;
    if (parser.isSet(QStringLiteral("paddleocr-vl-url")) &&
        !window.usePaddleVlService(QUrl(parser.value(QStringLiteral("paddleocr-vl-url"))))) {
        qCritical("Enter a valid HTTP(S) base URL for --paddleocr-vl-url.");
        return 2;
    }
    if (!parser.positionalArguments().isEmpty()) {
        const QString path = parser.positionalArguments().first();
        if (QFileInfo(path).isDir() || path.endsWith(QStringLiteral(".yagf.json"), Qt::CaseInsensitive))
            window.loadProject(path);
        else window.loadSource(path);
    }
    window.show();
    return QApplication::exec();
}
