#pragma once
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <chrono>
#include <expected>
#include <functional>

namespace yagf::ui {
class ReviewChat final : public QObject {
public:
    using Answer = std::expected<QString, QString>;
    struct Options {
        int maxTokens = 0;
        bool directAnswer = false;
    };
    explicit ReviewChat(std::chrono::milliseconds idleTimeout = std::chrono::minutes(2));
    ~ReviewChat() override { cancel(); }
    std::expected<void, QString> send(const QString& address, const QString& model, const QString& token,
        const QString& prompt, const QByteArray& png, Options options, std::function<void(Answer)> done,
        std::function<void(qint64)> receiving = {});
    void cancel();

private:
    void post(const QNetworkRequest& request, QJsonObject payload, std::function<void(Answer)> done,
        std::function<void(qint64)> receiving);
    QNetworkAccessManager network;
    QPointer<QNetworkReply> active;
    std::chrono::milliseconds idleTimeout;
};
}
