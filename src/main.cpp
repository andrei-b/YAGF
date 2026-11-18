#include "ui/mainwindow.h"

#include <QApplication>
#include <QFileInfo>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    yagf::ui::MainWindow window;
    if (argc > 1) {
        const QString path = QString::fromLocal8Bit(argv[1]);
        if (QFileInfo(path).isDir() || path.endsWith(QStringLiteral(".yagf.json"), Qt::CaseInsensitive))
            window.loadProject(path);
        else window.loadSource(path);
    }
    window.show();
    return app.exec();
}
