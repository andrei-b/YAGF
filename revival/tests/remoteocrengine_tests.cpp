#include "remoteocrengine.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf::processing;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTcpServer server;
    assert(server.listen(QHostAddress::LocalHost));
    int requests = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto *socket = server.nextPendingConnection();
        auto *requestBytes = new QByteArray;
        QObject::connect(socket, &QTcpSocket::disconnected, socket, [requestBytes, socket] {
            delete requestBytes;
            socket->deleteLater();
        });
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, requestBytes] {
            *requestBytes += socket->readAll();
            const int headerEnd = requestBytes->indexOf("\r\n\r\n");
            if (headerEnd < 0) return;
            const QByteArray headers = requestBytes->left(headerEnd);
            const int lengthStart = headers.indexOf("Content-Length: ");
            if (lengthStart < 0) return;
            const int lengthEnd = headers.indexOf("\r\n", lengthStart);
            const int length = headers.mid(lengthStart + 16,
                lengthEnd < 0 ? -1 : lengthEnd - lengthStart - 16).toInt();
            if (requestBytes->size() < headerEnd + 4 + length) return;
            const QByteArray request = *requestBytes;
            assert(request.contains("POST /v1/recognize HTTP/1.1"));
            assert(request.contains("Authorization: Bearer secret"));
            ++requests;
            if (requests == 3) return; // Keep a request in flight for cancellation.
            const QJsonObject response{
                {"protocol", "yagf-ocr/1"}, {"engine_id", "paddleocr"},
                {"width", 20}, {"height", 10}, {"language", "en"},
                {"lines", QJsonArray{QJsonObject{{"text", "Hello world"},
                    {"confidence", 0.9}, {"box", QJsonArray{1, 2, 12, 5}}}}}};
            QJsonObject sent = response;
            if (requests == 2) sent.insert(QStringLiteral("width"), 19);
            const QByteArray body = QJsonDocument(sent).toJson(QJsonDocument::Compact);
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                          QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });

    QImage image(20, 10, QImage::Format_RGB32);
    image.fill(Qt::white);
    RemoteOcrEngine engine(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())),
                           QStringLiteral("secret"));
    assert(!engine.capabilities().supportsWordBoxes && engine.capabilities().supportsConfidence);
    auto recognized = engine.recognize(image, {.language = QStringLiteral("en"), .pageNumber = 3}, {});
    assert(recognized && recognized->words.size() == 1);
    assert(recognized->words.first().bounds == QRect(1, 2, 12, 5));
    assert(recognized->words.first().text == QStringLiteral("Hello world"));
    assert(recognized->words.first().sourceBoxes == QVector<QRect>{QRect(1, 2, 12, 5)});
    assert(recognized->number == 3);
    auto invalid = engine.recognize(image, {.language = QStringLiteral("en")}, {});
    assert(!invalid && invalid.error().message.contains(QStringLiteral("schema")));
    std::stop_source stop;
    QTimer::singleShot(80, &server, [&] { stop.request_stop(); });
    auto cancelled = engine.recognize(image, {.language = QStringLiteral("en")}, stop.get_token());
    assert(!cancelled && cancelled.error().cancelled);
    assert(requests == 3);
}
