#include "aireviewdialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir directory;
    assert(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QTcpServer server;
    assert(server.listen(QHostAddress::LocalHost));
    int requests = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto *socket = server.nextPendingConnection();
        auto *received = new QByteArray;
        QObject::connect(socket, &QTcpSocket::disconnected, socket, [socket, received] {
            delete received;
            socket->deleteLater();
        });
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, received] {
            *received += socket->readAll();
            const int end = received->indexOf("\r\n\r\n");
            if (end < 0) return;
            const QByteArray headers = received->left(end);
            const int start = headers.indexOf("Content-Length: ");
            if (start < 0) return;
            const int finish = headers.indexOf("\r\n", start);
            const int length = headers.mid(start + 16,
                finish < 0 ? -1 : finish - start - 16).toInt();
            if (received->size() < end + 4 + length) return;
            ++requests;
            assert(received->contains("POST /v1/chat/completions HTTP/1.1"));
            assert(headers.contains("Authorization: Bearer secret"));
            const QJsonObject request = QJsonDocument::fromJson(received->mid(end + 4, length)).object();
            assert(request.value(QStringLiteral("model")).toString() == QStringLiteral("vision-test"));
            const QJsonArray messages = request.value(QStringLiteral("messages")).toArray();
            assert(messages.size() == 2);
            const QJsonArray content = messages.at(1).toObject().value(QStringLiteral("content")).toArray();
            assert(content.size() == 2);
            assert(content.at(0).toObject().value(QStringLiteral("text")).toString()
                .contains(QStringLiteral("nearby context")));
            assert(content.at(1).toObject().value(QStringLiteral("image_url")).toObject()
                .value(QStringLiteral("url")).toString().startsWith(QStringLiteral("data:image/png;base64,")));
            const QJsonObject message{{QStringLiteral("content"),
                QStringLiteral("{\"candidates\":[\"modernization\",\"modernisation\"]}")}};
            const QJsonObject response{{QStringLiteral("choices"),
                QJsonArray{QJsonObject{{QStringLiteral("message"), message}}}}};
            const QByteArray body = QJsonDocument(response).toJson(QJsonDocument::Compact);
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                          QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });

    QImage crop(80, 30, QImage::Format_RGB32);
    crop.fill(Qt::white);
    yagf::ui::AiReviewDialog dialog(QStringLiteral("modemization"),
        QStringLiteral("nearby context [modemization] matters"), crop, QRect(20, 5, 30, 15),
        QStringLiteral("secret"));
    dialog.setBookSuggestions({{"v", "ν", 2, false, 0, yagf::core::SymbolMatch::Similar}});
    auto *book = dialog.findChild<QComboBox *>(QStringLiteral("bookSuggestions"));
    assert(book && book->count() == 1 && book->itemText(0).contains("similar symbol image"));
    assert(book->currentIndex() == -1 && dialog.correction() == QStringLiteral("modemization"));
    auto *endpoint = dialog.findChild<QLineEdit *>(QStringLiteral("aiEndpoint"));
    auto *model = dialog.findChild<QLineEdit *>(QStringLiteral("aiModel"));
    auto *ask = dialog.findChild<QPushButton *>(QStringLiteral("askAiButton"));
    auto *suggestions = dialog.findChild<QListWidget *>(QStringLiteral("aiSuggestions"));
    assert(endpoint && model && ask && suggestions);
    endpoint->setText(QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(server.serverPort()));
    model->setText(QStringLiteral("vision-test"));
    dialog.show();
    app.processEvents();
    assert(requests == 0);
    for (int i = 0; i < 3; ++i) {
        auto *preview = dialog.findChild<QLabel *>(QStringLiteral("reviewPreview%1").arg(i));
        assert(preview && !preview->pixmap().isNull());
    }
    ask->click();
    QElapsedTimer timer;
    timer.start();
    while (suggestions->count() != 2 && timer.elapsed() < 3000) {
        app.processEvents();
        QThread::msleep(10);
    }
    assert(requests == 1 && suggestions->count() == 2);
    assert(dialog.correction() == QStringLiteral("modemization"));
    suggestions->setCurrentRow(1);
    assert(dialog.correction() == QStringLiteral("modernisation"));
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    assert(buttons);
    buttons->button(QDialogButtonBox::Ok)->click();
    assert(dialog.result() == QDialog::Accepted && dialog.sessionToken() == QStringLiteral("secret"));
}
