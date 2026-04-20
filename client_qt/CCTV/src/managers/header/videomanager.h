#ifndef VIDEOMANAGER_H
#define VIDEOMANAGER_H

#include <QObject>
#include <QThread>
#include <QByteArray>
#include <zmq.hpp>
#include "packetheader.h"

// =========================================================
// [데이터 수신부] ZmqReceiverWorker (포트 9002번)
// =========================================================
class ZmqReceiverWorker : public QObject {
    Q_OBJECT
public:
    explicit ZmqReceiverWorker(const QString& dataEndpoint, QObject* parent = nullptr);
    ~ZmqReceiverWorker();

public slots:
    void startReceiving();
    void stopReceiving();

signals:
    void frameReceived(int cameraId, const QByteArray& imageData);
    void logReady(QString message); // 👉 로그 전달용 시그널 추가

private:
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

    // 👉 추가: 9000번 포트로 VCR에 제어 명령(재생/탐색 등)을 요청하는 함수
    void sendControlCommand(int cameraId, int commandType, float value = 0.0f);

signals:
    void newFrame(int cameraId, QByteArray imageData);
    void logReady(QString message); // 👉 컨트롤러로 전달할 시그널 추가

private:
    QThread* m_workerThread;
    ZmqReceiverWorker* m_worker;
    QString m_controlEndpoint; // 제어용 9000번 포트 주소
};

#endif // VIDEOMANAGER_H