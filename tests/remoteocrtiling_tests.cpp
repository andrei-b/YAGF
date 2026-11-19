#include "remoteocrengine.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>
using namespace yagf::processing;
class TilingTests : public QObject {
    Q_OBJECT
private slots:
    void retryAndFailureBounds() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        int calls = 0, mode = 0;
        std::stop_source stop;
        connect(&server,&QTcpServer::newConnection,this,[&] {
            auto *socket = server.nextPendingConnection();
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,socket,[&,socket, bytes=QByteArray{}]() mutable {
                bytes += socket->readAll(); const int header = bytes.indexOf("\r\n\r\n"); if (header<0 || socket->property("sent").toBool()) return;
                const auto headers=bytes.left(header); int length=0;
                for (const auto &line:headers.split('\n')) if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
                if(bytes.size()<header+4+length)return;socket->setProperty("sent",true);++calls;
                if(mode==3 && calls==2){QTimer::singleShot(30,this,[&]{stop.request_stop();});return;}
                auto request=QJsonDocument::fromJson(bytes.mid(header+4,length)).object();
                QImage image; QVERIFY(image.loadFromData(QByteArray::fromBase64(request["image_png"].toString().toLatin1()),"PNG"));
                QJsonObject result; int status=200;
                if(calls==1 || mode==1){status=400;result={{"error","Dense page"},{"code","transcription_truncated"}};}
                else if(mode==2 && calls==3){status=500;result={{"error","Backend failed"}};}
                else {result={{"protocol","yagf-ocr/1"},{"engine_id","paddleocr"},{"width",image.width()},{"height",image.height()},
                     {"language","auto"},{"lines",QJsonArray{QJsonObject{{"text",calls==2?"Top passage":"Bottom passage"},{"confidence",QJsonValue::Null},
                     {"layout_block",true},{"box",QJsonArray{0,0,image.width(),image.height()}}}}}};}
                const auto payload=QJsonDocument(result).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 "+QByteArray::number(status)+" Result\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(payload.size())+"\r\nConnection: close\r\n\r\n"+payload);socket->disconnectFromHost();
            });
        });
        RemoteOcrEngine engine(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())));
        QImage image(320,400,QImage::Format_RGB32);image.fill(Qt::white);
        auto result=engine.recognize(image,{.language="auto",.pageNumber=2},{});QVERIFY(result.has_value());QCOMPARE(calls,3);QCOMPARE(result->words.size(),2);
        QCOMPARE(result->width,320);QCOMPARE(result->height,400);QCOMPARE(result->words[0].bounds,QRect(0,0,320,200));QCOMPARE(result->words[1].bounds,QRect(0,200,320,200));
        QCOMPARE(result->words[1].page,2);QVERIFY(result->words[0].id!=result->words[1].id);QVERIFY(!result->words[0].bounds.intersects(result->words[1].bounds));
        QCOMPARE(result->regions.size(),2);QCOMPARE(result->regions[1].bounds,result->words[1].bounds);
        calls=0;mode=1;result=engine.recognize(image,{.language="auto"},{});QVERIFY(!result);QCOMPARE(calls,3);
        calls=0;mode=2;result=engine.recognize(image,{.language="auto"},{});QVERIFY(!result);QCOMPARE(calls,3);QCOMPARE(result.error().message,"Backend failed");
        calls=0;mode=3;result=engine.recognize(image,{.language="auto"},stop.get_token());QVERIFY(!result && result.error().cancelled);QCOMPARE(calls,2);
    }
};
QTEST_GUILESS_MAIN(TilingTests)
#include "remoteocrtiling_tests.moc"
