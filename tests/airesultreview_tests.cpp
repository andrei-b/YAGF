#include "resultreviewdialog.h"
#include <QCheckBox>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QTextBrowser>
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
    std::function<void(QTcpSocket *)> respond;
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
                if (respond) {
                    respond(socket);
                    return;
                }
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
        dlg.findChild<QCheckBox *>("aiIncludeScan")->setChecked(vision);
        dlg.findChild<QCheckBox *>("aiDirectAnswer")->setChecked(!vision);
        dlg.show();
        auto *paper = dlg.findChild<QTextBrowser *>("aiReviewPaper");
        auto *apply = dlg.findChild<QPushButton *>("applyAiResult");
        QCOMPARE(server.payloads.size(), 0);
        dlg.findChild<QPushButton *>("askAiResult")->click();
        QTRY_COMPARE(server.payloads.size(), 2);
        QTRY_VERIFY(dlg.findChild<QLineEdit *>("aiModel")->isEnabled());
        const auto notes = dlg.findChildren<QCheckBox *>("aiNoteCheck");
        QCOMPARE(notes.size(), 2);
        QCOMPARE(paper->extraSelections().size(), 2);
        const auto coordinate = paper->extraSelections()[1].cursor;
        QCOMPARE(coordinate.selectedText(), QStringLiteral("2\u202fz"));
        const int coordinateStart = coordinate.selectionStart();
        QCOMPARE(coordinateStart, 13);
        auto formatting = coordinate;
        formatting.setPosition(coordinateStart + 1);
        QVERIFY(formatting.charFormat().fontStrikeOut());
        formatting.setPosition(coordinate.selectionEnd());
        QVERIFY(!formatting.charFormat().fontStrikeOut());
        QCOMPARE(formatting.charFormat().fontWeight(), QFont::DemiBold);
        const auto locate = dlg.findChildren<QPushButton *>("aiReviewSourceLink");
        QCOMPARE(locate.size(), 2);
        locate[1]->click();
        QCOMPARE(paper->textCursor().position(), coordinateStart);
        QVERIFY(dlg.findChild<QWidget *>("aiReviewScan")->isVisible());
        auto first = paper->extraSelections()[0].cursor;
        first.setPosition(first.selectionStart() + 1);
        const auto point = paper->cursorRect(first).center();
        QCOMPARE(paper->anchorAt(point), "note:0");
        QTest::mouseClick(paper->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(notes[0]->parentWidget()->property("active").toBool(), true);
        QVERIFY(!apply->isEnabled());
        QVERIFY(!dlg.edited);
        auto request = server.payloads.first();
        QVERIFY(request.value("stream").toBool());
        if (vision)
            QVERIFY(!request.contains("reasoning_effort"));
        else
            QCOMPARE(request.value("reasoning_effort").toString(), "none");
        const auto content = request.value("messages").toArray().at(1).toObject().value("content");
        QCOMPARE(content.isArray(), vision);
        auto prompt = vision ? content.toArray()[0].toObject()["text"].toString() : content.toString();
        QVERIFY(prompt.contains("modem"));
        QVERIFY(prompt.contains("0.0"));
        if (vision)
            QVERIFY(content.toArray()[1].toObject()["image_url"].toObject()["url"].toString().startsWith(
                "data:image/png;base64,"));
        notes[0]->setChecked(true);
        apply->click();
        QVERIFY(dlg.edited);
        QCOMPARE(dlg.edited->words[0].text, "modern");
        QCOMPARE(dlg.edited->words[0].originalText, "modem");
        QCOMPARE(dlg.edited->words[1].text, "2");
        QCOMPARE(dlg.edited->words[0].bounds, p.words[0].bounds);
        QCOMPARE(dlg.edited->words[0].readingEngine, "AI review · general-purpose-test");
    }
    void streamedReply_data()
    {
        QTest::addColumn<QByteArray>("ending");
        QTest::addColumn<QString>("expectedError");
        QTest::newRow("complete") << QByteArray("data: [DONE]\r\n\r\n") << QString();
        QTest::newRow("missing-end") << QByteArray(": closing\n\n") << QString("closed before");
        QTest::newRow("truncated") << QByteArray("data: {\"choices\":[{\"finish_reason\":\"length\"}]}\n\n")
                                    << QString("truncated");
        QTest::newRow("stalled") << QByteArray() << QString("timed out");
    }
    void streamedReply()
    {
        QFETCH(QByteArray, ending);
        QFETCH(QString, expectedError);
        Server server;
        QVERIFY(server.server.listen(QHostAddress::LocalHost));
        server.respond = [ending](QTcpSocket *socket) {
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
                          "Connection: close\r\n\r\n"
                          "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"private\"}}]}\r\n\r\n");
            if (ending.isEmpty())
                return;
            // Each gap is short, but completing the answer takes longer than the idle timeout.
            QTimer::singleShot(150, socket, [socket] {
                socket->write("data: {\"choices\":[{\"delta\":{\"content\":\"cor");
            });
            QTimer::singleShot(300, socket, [socket] {
                socket->write("rect\"}}]}\n\ndata: {\"choices\":[{\"delta\":{\"content\":\"ed\"}}]}\n\n");
            });
            QTimer::singleShot(450, socket, [socket, ending] {
                socket->write(ending);
                socket->disconnectFromHost();
            });
        };
        ui::ReviewChat chat(std::chrono::milliseconds(350));
        std::optional<ui::ReviewChat::Answer> answer;
        const auto url = QString("http://127.0.0.1:%1").arg(server.server.serverPort());
        QVERIFY(chat.send(url, "test", {}, "Review", {}, {}, [&](auto result) { answer = std::move(result); }));
        QTRY_VERIFY(answer.has_value());
        if (expectedError.isEmpty()) {
            QVERIFY(answer->has_value());
            QCOMPARE(**answer, "corrected");
        } else {
            QVERIFY(!answer->has_value());
            QVERIFY2(answer->error().contains(expectedError), qPrintable(answer->error()));
        }
    }
    void serverRejectsStreaming()
    {
        Server server;
        QVERIFY(server.server.listen(QHostAddress::LocalHost));
        server.respond = [&server](QTcpSocket *socket) {
            if (server.payloads.last().value("stream").toBool()) {
                socket->write("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"
                              "{\"error\":{\"message\":\"Streaming is not supported\"}}");
            } else {
                socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"
                              "{\"choices\":[{\"message\":{\"content\":\"reply\"}}]}");
            }
            socket->disconnectFromHost();
        };
        ui::ReviewChat chat;
        std::optional<ui::ReviewChat::Answer> answer;
        const auto url = QString("http://127.0.0.1:%1").arg(server.server.serverPort());
        QVERIFY(chat.send(url, "test", {}, "Review", {}, {}, [&](auto result) { answer = std::move(result); }));
        QTRY_VERIFY(answer.has_value());
        QVERIFY(answer->has_value());
        QCOMPARE(**answer, "reply");
        QCOMPARE(server.payloads.size(), 2);
        auto firstRequest = server.payloads.first();
        firstRequest.insert("stream", false);
        QCOMPARE(server.payloads.last(), firstRequest);
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
