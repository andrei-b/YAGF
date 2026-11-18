#include "ui/mainwindow.h"

#include <QApplication>
#include <QFileInfo>
#include <QIcon>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setDesktopFileName(QStringLiteral("io.github.andrei_b.YAGF"));
    QIcon icon(QStringLiteral(":/legacy/yagf-48.png"));
    icon.addFile(QStringLiteral(":/legacy/yagf.png"));
    QApplication::setWindowIcon(icon);

    yagf::ui::MainWindow window;
    if (argc > 1) {
        const QString path = QString::fromLocal8Bit(argv[1]);
        if (QFileInfo(path).isDir() || path.endsWith(QStringLiteral(".yagf.json"), Qt::CaseInsensitive))
            window.loadProject(path);
        else window.loadSource(path);
    }
    window.show();
    return QApplication::exec();
}
