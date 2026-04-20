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

    // 1. 연결 시도 로그
    QString startMsg = QString("[Data] VCR 스트리밍 서버 연결 시도 중... -> %1").arg(m_dataEndpoint);
    qDebug() << startMsg;
    emit logReady(startMsg); 

    socket.connect(m_dataEndpoint.toStdString());
    socket.set(zmq::sockopt::subscribe, "");

    int timeout = 500; // 0.5초
    socket.set(zmq::sockopt::rcvtimeo, timeout);

    // 💡 변수 선언 완벽 정리
    int silenceCounter = 0;   // 데이터 미수신 횟수 카운터
    bool isConnected = false; // 실제 데이터 수신 여부 플래그
    int debugCounter = 0;     // 30프레임 확인용 디버그 카운터 (추가됨)

    while (m_running) {
        zmq::message_t topic_msg, header_msg, payload_msg;

        // 1. Topic 수신 시도
        auto r1 = socket.recv(topic_msg, zmq::recv_flags::none);

        if (!r1) { // 500ms 동안 데이터가 오지 않음
            silenceCounter++;
            
            // 0.5초 * 6번 = 3초 동안 데이터가 없으면 연결 실패/끊김으로 판단
            if (silenceCounter >= 6 && isConnected) {
                QString failMsg = "[Data 에러] VCR 서버와 연결이 끊어졌습니다. (데이터 미수신)";
                qDebug() << failMsg;
                emit logReady(failMsg);
                isConnected = false; // 상태 변경
            } else if (silenceCounter == 6 && !isConnected) {
                // 처음부터 연결이 안 된 경우
                QString failMsg = "[Data 에러] VCR 서버(9002)가 닫혀있거나 응답이 없습니다.";
                qDebug() << failMsg;
                emit logReady(failMsg);
            }
            continue; 
        }

        // 💡 수정됨: timeoutCounter -> silenceCounter로 통일
        // 프레임 정상 수신 시 카운터 리셋 및 연결 복구 로그
        if (!isConnected && silenceCounter > 0) {
            emit logReady("[Data] VCR 영상 스트리밍 수신 재개됨!");
        }
        isConnected = true;
        silenceCounter = 0; // 카운터 초기화

        if (!topic_msg.more()) continue;

        // 2. Header 수신 
        auto r2 = socket.recv(header_msg, zmq::recv_flags::none);
        if (!r2 || !header_msg.more()) continue;

        // 3. Payload (JPEG 데이터) 수신 
        auto r3 = socket.recv(payload_msg, zmq::recv_flags::none);
        if (!r3) continue;

        // 💡 수정됨: 프로그램 크래시 방지를 위한 안전장치(예외 처리) 복구
        if (header_msg.size() != sizeof(ViewerPacketHeader)) {
            qDebug() << "헤더 크기 불일치! 받은 크기:" << header_msg.size() 
                     << "예상 크기:" << sizeof(ViewerPacketHeader);
            continue; // 비정상 패킷은 스킵 (강제 종료 방지)
        }

        // 데이터 검증 및 처리
        if (header_msg.size() == sizeof(ViewerPacketHeader)) {
            auto* header = static_cast<ViewerPacketHeader*>(header_msg.data());
            
            // ========================================================
            // 수신된 토픽과 ID를 30프레임마다 출력
            debugCounter++;
            if (debugCounter % 15 == 0) { 
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