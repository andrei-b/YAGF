#pragma once
#include <QNetworkAccessManager>
#include <QPointer>
#include <expected>
#include <functional>

namespace yagf::ui {
class ReviewChat final : public QObject {
public:
    using Answer = std::expected<QString, QString>;
    using QObject::QObject;
    ~ReviewChat() override { cancel(); }
    std::expected<void, QString> send(const QString& address, const QString& model, const QString& token,
        const QString& prompt, const QByteArray& png, int maxTokens, std::function<void(Answer)> done);
    void cancel();

private:
    QNetworkAccessManager network;
    QPointer<QNetworkReply> active;
};
}
