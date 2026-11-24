#include "resultreviewdialog.h"
#include <QCheckBox>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

using namespace yagf;
namespace {
core::OcrPage sample(int count = 2)
{
    core::OcrPage p;
    p.width = 200;
    p.height = 100;
    for (int i = 0; i < count; ++i) {
        core::OcrWord w;
        w.text = w.originalText = i == 0 ? "modem" : "2";
        w.bounds = QRect(2 + (i % 16) * 12, 5 + (i / 16) * 18, 10, 12);
        w.line = 1 + i / 16;
        w.block = w.paragraph = 1;
        p.words.append(w);
    }
    p.rebuildHierarchy();
    return p;
}
class Server {
public:
    QTcpServer server;
    QVector<QJsonObject> payloads;
    QStringList answers;
    Server()
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            auto *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                auto data = socket->property("buffer").toByteArray() + socket->readAll();
                socket->setProperty("buffer", data);
                const auto split = data.indexOf("\r\n\r\n");
                if (split < 0)
                    return;
                const auto header = data.left(split).toLower();
                const int start = header.indexOf("content-length: ") + 16;
                const auto length = header.mid(start).split('\r').first().toInt();
                if (data.size() < split + 4 + length || socket->property("responded").toBool())
                    return;
                socket->setProperty("responded", true);
                payloads.append(QJsonDocument::fromJson(data.mid(split + 4, length)).object());
                const auto text = answers.isEmpty() ? QStringLiteral("{\"changes\":[]}") : answers.takeFirst();
                const auto response = QJsonDocument(
                    QJsonObject { { "choices",
                        QJsonArray { QJsonObject { { "message", QJsonObject { { "content", text } } } } } } })
                                          .toJson();
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(response.size())
                    + "\r\nConnection: close\r\n\r\n" + response);
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
};
}
class AiResultTests : public QObject {
    Q_OBJECT
private slots:
    void review_data()
    {
        QTest::addColumn<bool>("vision");
        QTest::newRow("text-only") << false;
        QTest::newRow("vision") << true;
    }
    void review()
    {
        QFETCH(bool, vision);
        QTemporaryDir tmp;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tmp.path());
        Server server;
        QVERIFY(server.server.listen(QHostAddress::LocalHost));
        server.answers = {
            R"({"changes":[{"id":"0.0","before":"modem","after":"modern","reason":"Glyphs rn resemble m."},{"id":"1.0","before":"2","after":"z","reason":"Coordinate."}]})"
        };
        auto p = sample(50);
        QVector<int> words;
        for (int i = 0; i < 50; ++i)
            words.append(i);
        QImage scan(p.width, p.height, QImage::Format_RGB32);
        scan.fill(Qt::white);
        ui::ResultReviewDialog dlg(p, words, scan, false, {}, nullptr);
        dlg.findChild<QLineEdit *>("aiEndpoint")
            ->setText(QString("http://127.0.0.1:%1/v1/chat/completions").arg(server.server.serverPort()));
        dlg.findChild<QLineEdit *>("aiModel")->setText("general-purpose-test");
        dlg.findChild<QCheckBox *>()->setChecked(vision);
        dlg.show();
        auto *list = dlg.findChild<QTreeWidget *>("aiResultChanges");
        auto *apply = dlg.findChild<QPushButton *>("applyAiResult");
        QCOMPARE(server.payloads.size(), 0);
        dlg.findChild<QPushButton *>("askAiResult")->click();
        QTRY_COMPARE(server.payloads.size(), 2);
        QTRY_VERIFY(dlg.findChild<QLineEdit *>("aiModel")->isEnabled());
        QCOMPARE(list->topLevelItemCount(), 2);
        QVERIFY(!apply->isEnabled());
        QVERIFY(!dlg.edited);
        auto request = server.payloads.first();
        const auto content = request.value("messages").toArray().at(1).toObject().value("content");
        QCOMPARE(content.isArray(), vision);
        auto prompt = vision ? content.toArray()[0].toObject()["text"].toString() : content.toString();
        QVERIFY(prompt.contains("modem"));
        QVERIFY(prompt.contains("0.0"));
        if (vision)
            QVERIFY(content.toArray()[1].toObject()["image_url"].toObject()["url"].toString().startsWith(
                "data:image/png;base64,"));
        list->topLevelItem(0)->setCheckState(0, Qt::Checked);
        apply->click();
        QVERIFY(dlg.edited);
        QCOMPARE(dlg.edited->words[0].text, "modern");
        QCOMPARE(dlg.edited->words[0].originalText, "modem");
        QCOMPARE(dlg.edited->words[1].text, "2");
        QCOMPARE(dlg.edited->words[0].bounds, p.words[0].bounds);
        QCOMPARE(dlg.edited->words[0].readingEngine, "AI review · general-purpose-test");
    }
    void exactSourceAndStructure()
    {
        auto p = sample();
        auto &w = p.words[0];
        w.layoutBlock = true;
        w.text = "2\tx\nx\tZ";
        w.originalText = w.text;
        const auto batches = intelligence::reviewBatches(p, { 0 });
        QVERIFY(
            !intelligence::readChanges(batches[0], R"({"changes":[{"id":"0.0","before":"x","after":"y"}]})"));
        QVERIFY(!intelligence::readChanges(
            batches[0], R"({"changes":[{"id":"unknown","before":"Z","after":"z"}]})"));
        QVERIFY(!intelligence::readChanges(
            batches[0], R"({"changes":[{"id":"0.0","before":"2\tx","after":"z"}]})"));
        auto ok
            = intelligence::readChanges(batches[0], R"({"changes":[{"id":"0.0","before":"Z","after":"z"}]})");
        QVERIFY(ok);
        auto result = intelligence::applyChanges(p, *ok, "test", false);
        QVERIFY(result);
        QCOMPARE(result->words[0].text, "2\tx\nx\tz");
        QVERIFY(result->words[0].layoutBlock);
        const auto deletion
            = intelligence::readChanges(batches[0], R"({"changes":[{"id":"0.0","before":"Z","after":""}]})");
        QVERIFY(deletion);
        QCOMPARE(
            intelligence::applyChanges(p, *deletion, "test", false)->words.first().text, QString("2\tx\nx\t"));
        QVERIFY(!intelligence::readChanges(batches[0], R"({"changes":[{"id":"0.0","before":"Z"}]})"));
        p.words[0].text = "changed";
        QVERIFY(!intelligence::applyChanges(p, *ok, "test", false));
    }
};
QTEST_MAIN(AiResultTests)
#include "airesultreview_tests.moc"
