#include "mainserverclient.h"
#include <QDebug>

MainServerClient::MainServerClient(QObject *parent) : QObject(parent) {
    connect(&m_webSocket, &QWebSocket::connected, this, &MainServerClient::onConnected);
    connect(&m_webSocket, &QWebSocket::textMessageReceived, this, &MainServerClient::onTextMessageReceived);
    connect(&m_webSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::errorOccurred),
            this, &MainServerClient::onError);
}

void MainServerClient::connectToWebSocket(const QString &url) {
    m_webSocket.open(QUrl(url));
}

void MainServerClient::onConnected() {
    emit logReady("[WebSocket] 메인 서버 알람 채널 연결 성공!");
}

void MainServerClient::onTextMessageReceived(QString message) {
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isNull() && doc.isObject()) {
        QJsonObject obj = doc.object();
        if (obj["event"].toString() == "FALL") {
            int camId = obj["cam_id"].toInt();
            int fallId = obj["fall_id"].toInt();
            emit emergencyFallDetected(camId, fallId);
        }
    }
}

void MainServerClient::onError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error);
    emit logReady("[WebSocket] 에러 발생: " + m_webSocket.errorString());
}