#ifndef MAINSERVERTCPCLIENT_H
#define MAINSERVERTCPCLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>
#include <QVariantList>

class MainServerTcpClient : public QObject {
    Q_OBJECT
public:
    explicit MainServerTcpClient(QObject *parent = nullptr);
    ~MainServerTcpClient();

    void connectToServer(const QString& ip, quint16 port);
    void requestEventList(int camId, qint64 startTime, qint64 endTime);
    void requestVideoUrl(int camId, qint64 eventId);

signals:
    void eventListReceived(QVariantList list);
    void videoUrlReceived(QString url);
    void logReady(QString message);

private slots:
    void onReadyRead();
    void onConnected();
    void onDisconnected();

private:
    QTcpSocket* m_socket;
    QByteArray m_buffer; // TCP 조각난 데이터 병합용 버퍼
};

#endif // MAINSERVERTCPCLIENT_H