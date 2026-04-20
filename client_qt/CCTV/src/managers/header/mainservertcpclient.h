// src/managers/header/mainservertcpclient.h
#ifndef MAINSERVERTCPCLIENT_H
#define MAINSERVERTCPCLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QJsonArray>
#include <QVariantList>

class MainServerTcpClient : public QObject {
    Q_OBJECT
public:
    explicit MainServerTcpClient(QObject *parent = nullptr);
    void connectToServer(const QString &ip, quint16 port);

    // QML에서 호출할 수 있는 요청 함수들
    Q_INVOKABLE void requestEventList(int camId, qint64 startTime, qint64 endTime);
    Q_INVOKABLE void requestVideoUrl(int camId, qint64 eventId);

signals:
    void logReady(QString message);
    void eventListReceived(QVariantList eventList); // QML 리스트뷰 갱신용
    void videoUrlReceived(QString streamUrl);       // QML 비디오 플레이어 재생용

private slots:
    void onConnected();
    void onReadyRead();
    void onError(QAbstractSocket::SocketError socketError);

private:
    QTcpSocket m_socket;
    
    // 수신 버퍼 및 상태 관리 (가변 길이 페이로드 처리를 위함)
    QByteArray m_buffer;
    bool m_isReadingPayload = false;
    uint16_t m_currentMsgId = 0;
    uint32_t m_expectedPayloadSize = 0;
    uint8_t m_currentStatusCode = 0;
};

#endif // MAINSERVERTCPCLIENT_H