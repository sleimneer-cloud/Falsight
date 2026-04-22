#ifndef SYSTEMCONTROLLER_H
#define SYSTEMCONTROLLER_H

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QWebSocket>
#include <QVariantList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QImage>

// 전방 선언
class VideoManager;
class VideoImageProvider;
class DatabaseManager;
class ArduinoManager;
class MainServerTcpClient;

class SystemController : public QObject {
    Q_OBJECT
public:
    explicit SystemController(QObject *parent = nullptr);
    ~SystemController();

    Q_INVOKABLE void requestLogin(const QString &id, const QString &pw);
    Q_INVOKABLE void seekVideo(int chIndex, float position);

    Q_INVOKABLE void fetchEventList(int camId, qint64 startTime, qint64 endTime);
    Q_INVOKABLE void fetchVideoUrl(int camId, qint64 eventId);
    Q_INVOKABLE void downloadVideo(const QString &urlStr, const QString &fileName);
    Q_INVOKABLE void stopArduinoAlarm();

    VideoImageProvider* getImageProvider() const { return m_imageProvider; }

signals:
    void loginResult(bool success, QString message);
    void frameUpdated(int cameraId);
    void systemLog(QString time, QString message); 
    void eventListUpdated(QVariantList eventList); 
    void videoUrlReady(QString streamUrl);         
    
    // 👉 여기에 시그널이 정상적으로 포함됩니다!
    void emergencyFallDetected(int camId, int fallId);

public slots:
    void onNewFrameReceived(int cameraId, QImage image); // QImage로 통일
    void onLogReady(QString message);

private slots:
    void onWsConnected();
    void onWsDisconnected();
    void onWsTextMessageReceived(QString message);
    void onDownloadFinished(QNetworkReply *reply);

private:
    VideoManager* m_videoManager;
    VideoImageProvider* m_imageProvider;
    DatabaseManager* m_dbManager;
    ArduinoManager* m_arduinoManager;
    QWebSocket* m_webSocket;
    MainServerTcpClient* m_tcpClient; 

    QNetworkAccessManager* m_netManager;
};

#endif // SYSTEMCONTROLLER_H