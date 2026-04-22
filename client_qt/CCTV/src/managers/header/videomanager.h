#ifndef VIDEOMANAGER_H
#define VIDEOMANAGER_H

#include <QObject>
#include <QThread>
#include <QByteArray>
#include <QList>      // 👉 다중 스레드 관리를 위해 추가
#include <QImage>     
#include <zmq.hpp>
#include "packetheader.h"

// =========================================================
// [데이터 수신부] ZmqReceiverWorker (각 카메라 포트별 할당)
// =========================================================
class ZmqReceiverWorker : public QObject {
    Q_OBJECT
public:
    // 👉 cameraId를 추가로 받도록 수정
    explicit ZmqReceiverWorker(int cameraId, const QString& dataEndpoint, QObject* parent = nullptr);
    ~ZmqReceiverWorker();

public slots:
    void startReceiving();
    void stopReceiving();

signals:
    void frameReceived(int cameraId, QImage image); 
    void logReady(QString message); 

private:
    int m_cameraId;       // 👉 현재 워커가 담당하는 카메라 ID
    QString m_dataEndpoint;
    bool m_running;
};

// =========================================================
// [제어 관리 및 브릿지] VideoManager 
// =========================================================
class VideoManager : public QObject {
    Q_OBJECT
public:
    explicit VideoManager(const QString& controlEndpoint, QObject* parent = nullptr);
    ~VideoManager();

    Q_INVOKABLE void startStreaming();
    Q_INVOKABLE void stopStreaming();

    void sendControlCommand(int cameraId, int commandType, float value = 0.0f);

signals:
    void newFrame(int cameraId, QImage image);
    void logReady(QString message); 

private:
    // 👉 단일 객체에서 리스트로 변경 (카메라 대수만큼 스레드 생성)
    QList<QThread*> m_workerThreads;
    QList<ZmqReceiverWorker*> m_workers;
    
    QString m_controlEndpoint; 
};

#endif // VIDEOMANAGER_H