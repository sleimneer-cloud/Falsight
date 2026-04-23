#ifndef MAINSERVERCLIENT_H
#define MAINSERVERCLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>

class MainServerClient : public QObject {
    Q_OBJECT
public:
    explicit MainServerClient(QObject *parent = nullptr);
    void connectToWebSocket(const QString &url);

signals:
    void logReady(QString message);
    void emergencyFallDetected(int camId, int fallId); // 낙상 감지 시 시그널

private slots:
    void onConnected();
    void onTextMessageReceived(QString message);
    void onError(QAbstractSocket::SocketError error);

private:
    QWebSocket m_webSocket;
};

#endif // MAINSERVERCLIENT_H