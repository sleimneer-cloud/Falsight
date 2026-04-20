#include "videomanager.h"
#include "packetheader.h" // 👉 구조체 인식을 위해 필수
#include <QDebug>
#include <QTcpSocket>
#include <QtEndian> // Big-Endian 변환용

// =========================================================
// ZmqReceiverWorker (실시간 스트리밍 수신 - 9002번 포트)
// =========================================================
ZmqReceiverWorker::ZmqReceiverWorker(const QString& dataEndpoint, QObject* parent)
    : QObject(parent), m_dataEndpoint(dataEndpoint), m_running(false) {}

ZmqReceiverWorker::~ZmqReceiverWorker() {
    stopReceiving();
}

void ZmqReceiverWorker::startReceiving() {
    m_running = true;
    zmq::context_t context(1);
    zmq::socket_t socket(context, ZMQ_SUB);

    socket.connect(m_dataEndpoint.toStdString()); // 9002번 포트
    socket.set(zmq::sockopt::subscribe, ""); // 모든 cam 토픽 수신

    // 수신 대기 시간을 500ms로 설정 (무한 대기 방지)
    int timeout = 500;
    socket.set(zmq::sockopt::rcvtimeo, timeout);

    QString logMsg = QString("[Data] ZMQ PULL 스트리밍 접속 완료 -> %1").arg(m_dataEndpoint);
    qDebug() << logMsg;
    emit logReady(logMsg); 

    // 👉 추가: 로그 폭주 방지용 카운터
    int debugCounter = 0;

    while (m_running) {
        zmq::message_t topic_msg, header_msg, payload_msg;

        // 1. Topic 수신
        auto r1 = socket.recv(topic_msg, zmq::recv_flags::none);
        if (!r1) continue; 
        if (!topic_msg.more()) continue;

        // 2. Header (20 또는 24 Bytes) 수신 
        auto r2 = socket.recv(header_msg, zmq::recv_flags::none);
        if (!r2 || !header_msg.more()) continue;

        // 3. Payload (JPEG 데이터) 수신 
        auto r3 = socket.recv(payload_msg, zmq::recv_flags::none);
        if (!r3) continue;

        if (header_msg.size() != sizeof(ViewerPacketHeader)) {
            qDebug() << "헤더 크기 불일치! 받은 크기:" << header_msg.size() 
                     << "예상 크기:" << sizeof(ViewerPacketHeader);
        }

        // 데이터 검증 및 처리
        if (header_msg.size() == sizeof(ViewerPacketHeader)) {
            auto* header = static_cast<ViewerPacketHeader*>(header_msg.data());
            
            // ========================================================
            // 👉 추가된 핵심 코드: 수신된 토픽과 ID를 30프레임마다 출력
            debugCounter++;
            if (debugCounter % 30 == 0) { 
                QString rcvTopic = QString::fromStdString(std::string(static_cast<const char*>(topic_msg.data()), topic_msg.size()));
                qDebug() << "👀 [크로스체크] 수신된 토픽명:" << rcvTopic << "| 구조체 내 카메라 ID:" << header->camera_id;
            }
            // ========================================================

            QByteArray frameData(static_cast<const char*>(payload_msg.data()), 
                                 static_cast<int>(payload_msg.size()));
            
            emit frameReceived(header->camera_id, frameData);
        }
    }
    
    qDebug() << "[VideoManager] ZMQ Receiver Thread Safely Stopped.";
}

void ZmqReceiverWorker::stopReceiving() {
    m_running = false;
}

// =========================================================
// VideoManager (제어부 - 9000번 포트)
// =========================================================
VideoManager::VideoManager(const QString& controlEndpoint, QObject* parent) 
    : QObject(parent), m_controlEndpoint(controlEndpoint) 
{
    m_workerThread = new QThread(this);
    
    // 스트리밍 워커에는 9002번 포트를 전달
    m_worker = new ZmqReceiverWorker("tcp://10.10.10.100:9002");
    m_worker->moveToThread(m_workerThread);

    connect(m_workerThread, &QThread::started, m_worker, &ZmqReceiverWorker::startReceiving);
    connect(m_worker, &ZmqReceiverWorker::frameReceived, this, &VideoManager::newFrame);
    connect(m_worker, &ZmqReceiverWorker::logReady, this, &VideoManager::logReady);
}

VideoManager::~VideoManager() {
    stopStreaming();
    m_workerThread->quit();
    m_workerThread->wait();
}

void VideoManager::startStreaming() {
    if (!m_workerThread->isRunning()) {
        m_workerThread->start();
    }
}

void VideoManager::stopStreaming() {
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "stopReceiving", Qt::QueuedConnection);
    }
}

void VideoManager::sendControlCommand(int cameraId, int commandType, float value) {
    QTcpSocket tcpSocket;
    tcpSocket.connectToHost("10.10.10.100", 9000); 

    if (tcpSocket.waitForConnected(3000)) {
        QString logMsg = QString("[Control] VCR 제어 연결 성공 (10.10.10.100:9000)");
        emit logReady(logMsg); 
        VcrControlRequest req;
        
        req.message_id = qToBigEndian<uint16_t>(300);
        req.camera_id = static_cast<uint8_t>(cameraId);
        req.request_type = static_cast<uint8_t>(commandType);
        
        uint64_t targetTime = static_cast<uint64_t>(value); 
        req.start_time = qToBigEndian<uint64_t>(targetTime);
        req.end_time = qToBigEndian<uint64_t>(targetTime + 10000); 

        tcpSocket.write(reinterpret_cast<const char*>(&req), sizeof(VcrControlRequest));
        if (tcpSocket.waitForBytesWritten(500)) {
            emit logReady(QString("[Control] CAM %1 영상 탐색 명령 전송 완료 (ID: 300)").arg(cameraId));
        }
        tcpSocket.disconnectFromHost();
    } else {
        QString errorMsg = "[Control] VCR TCP 제어 포트(9000) 연결 실패!";
        qDebug() << errorMsg;
        emit logReady(errorMsg); 
    }
}