#ifndef SYSTEMCONTROLLER_H
#define SYSTEMCONTROLLER_H

#include <QObject>
#include <QString>
#include <QByteArray>

// 전방 선언
class VideoManager;
class VideoImageProvider;
class DatabaseManager; // 👉 날아갔던 DB 매니저 복구!
class ArduinoManager;

class SystemController : public QObject {
    Q_OBJECT
public:
    explicit SystemController(QObject *parent = nullptr);
    ~SystemController(); // 👉 날아갔던 소멸자 복구!

    Q_INVOKABLE void requestLogin(const QString &id, const QString &pw);
    Q_INVOKABLE void seekVideo(int chIndex, float position);

    VideoImageProvider* getImageProvider() const { return m_imageProvider; }

signals:
    void loginResult(bool success, QString message);
    void frameUpdated(int cameraId);
    void systemLog(QString time, QString message); // 👉 QML로 쏠 최종 시그널

public slots:
    void onNewFrameReceived(int cameraId, QByteArray imageData);
    void onLogReady(QString message); // 👉 VideoManager에서 오는 로그 받는 슬롯

private:
    VideoManager* m_videoManager;
    VideoImageProvider* m_imageProvider;
    DatabaseManager* m_dbManager; // 👉 날아갔던 DB 매니저 복구!
    ArduinoManager* m_arduinoManager; // 👉 추가
};

#endif // SYSTEMCONTROLLER_H