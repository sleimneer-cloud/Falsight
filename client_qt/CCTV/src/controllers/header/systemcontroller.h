// src/controllers/header/systemcontroller.h
#ifndef SYSTEMCONTROLLER_H
#define SYSTEMCONTROLLER_H

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QWebSocket>
#include <QVariantList>
#include <QNetworkAccessManager> // 👉 다운로드 매니저 추가
#include <QNetworkReply>

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

    // ==========================================================
    // 👉 QML에서 호출할 영상 다운로드 함수 추가
    // ==========================================================
    Q_INVOKABLE void downloadVideo(const QString &urlStr, const QString &fileName);

    VideoImageProvider* getImageProvider() const { return m_imageProvider; }

signals:
    void loginResult(bool success, QString message);
    void frameUpdated(int cameraId);
    void systemLog(QString time, QString message); 
    
    void eventListUpdated(QVariantList eventList); 
    void videoUrlReady(QString streamUrl);         

public slots:
    void onNewFrameReceived(int cameraId, QByteArray imageData);
    void onLogReady(QString message);

private slots:
    void onWsConnected();
    void onWsDisconnected();
    void onWsTextMessageReceived(QString message);
    
    // 👉 다운로드 완료 처리를 위한 슬롯 추가
    void onDownloadFinished(QNetworkReply *reply);

private:
    VideoManager* m_videoManager;
    VideoImageProvider* m_imageProvider;
    DatabaseManager* m_dbManager;
    ArduinoManager* m_arduinoManager;
    QWebSocket* m_webSocket;
    MainServerTcpClient* m_tcpClient; 

    // 👉 파일 다운로드 매니저 객체
    QNetworkAccessManager* m_netManager;
};

#endif // SYSTEMCONTROLLER_H// src/controllers/header/systemcontroller.h
#ifndef SYSTEMCONTROLLER_H
#define SYSTEMCONTROLLER_H

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QWebSocket>
#include <QVariantList>
#include <QNetworkAccessManager> // 👉 다운로드 매니저 추가
#include <QNetworkReply>

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

    // ==========================================================
    // 👉 QML에서 호출할 영상 다운로드 함수 추가
    // ==========================================================
    Q_INVOKABLE void downloadVideo(const QString &urlStr, const QString &fileName);

    VideoImageProvider* getImageProvider() const { return m_imageProvider; }

signals:
    void loginResult(bool success, QString message);
    void frameUpdated(int cameraId);
    void systemLog(QString time, QString message); 
    
    void eventListUpdated(QVariantList eventList); 
    void videoUrlReady(QString streamUrl);         

public slots:
    void onNewFrameReceived(int cameraId, QByteArray imageData);
    void onLogReady(QString message);

private slots:
    void onWsConnected();
    void onWsDisconnected();
    void onWsTextMessageReceived(QString message);
    
    // 👉 다운로드 완료 처리를 위한 슬롯 추가
    void onDownloadFinished(QNetworkReply *reply);

private:
    VideoManager* m_videoManager;
    VideoImageProvider* m_imageProvider;
    DatabaseManager* m_dbManager;
    ArduinoManager* m_arduinoManager;
    QWebSocket* m_webSocket;
    MainServerTcpClient* m_tcpClient; 

    // 👉 파일 다운로드 매니저 객체
    QNetworkAccessManager* m_netManager;
};

#endif // SYSTEMCONTROLLER_H