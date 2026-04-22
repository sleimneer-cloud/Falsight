#include "videomanager.h"
#include "packetheader.h" 
#include <QDebug>
#include <QtEndian> 
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>

// =========================================================
// ZmqReceiverWorker (포트별 독립 수신)
// =========================================================
ZmqReceiverWorker::ZmqReceiverWorker(int cameraId, const QString& dataEndpoint, QObject* parent)
    : QObject(parent), m_cameraId(cameraId), m_dataEndpoint(dataEndpoint), m_running(false) {}

ZmqReceiverWorker::~ZmqReceiverWorker() {
    stopReceiving();
}

void ZmqReceiverWorker::startReceiving() {
    m_running = true;
    zmq::context_t context(1);
    zmq::socket_t socket(context, ZMQ_SUB);

    QString startMsg = QString("[Data] CAM %1 스트리밍 연결 시도... -> %2").arg(m_cameraId).arg(m_dataEndpoint);
    qDebug() << startMsg;
    emit logReady(startMsg); 

    socket.connect(m_dataEndpoint.toStdString());
    
    // 👉 포트별로 분리되었으므로, 해당 포트로 들어오는 "모든 데이터"를 구독 ("" 지정)
    socket.set(zmq::sockopt::subscribe, ""); 

    int timeout = 500; 
    socket.set(zmq::sockopt::rcvtimeo, timeout);

    int silenceCounter = 0;   
    bool isConnected = false;  

    while (m_running) {
        zmq::message_t part1, part2, part3;

        // 1. 첫 번째 파트 수신
        auto r1 = socket.recv(part1, zmq::recv_flags::none);

        if (!r1) { 
            silenceCounter++;
            if (silenceCounter >= 6 && isConnected) {
                QString failMsg = QString("[Data 에러] CAM %1 연결 끊김 (데이터 미수신)").arg(m_cameraId);
                emit logReady(failMsg);
                isConnected = false; 
            } else if (silenceCounter == 6 && !isConnected) {
                QString failMsg = QString("[Data 에러] CAM %1 서버(%2) 응답 없음").arg(m_cameraId).arg(m_dataEndpoint);
                emit logReady(failMsg);
            }
            continue; 
        }

        if (!isConnected && silenceCounter > 0) {
            emit logReady(QString("[Data] CAM %1 수신 재개됨!").arg(m_cameraId));
        }
        isConnected = true;
        silenceCounter = 0; 

        if (!part1.more()) continue;

        // 2. 두 번째 파트 수신
        auto r2 = socket.recv(part2, zmq::recv_flags::none);
        if (!r2) continue;

        zmq::message_t header_msg;
        zmq::message_t payload_msg;

        // 💡 VCR 서버 변경 대응: 
        // 기존처럼 3단(Topic->Header->Payload)으로 오는지,
        // 포트가 분리되어 2단(Header->Payload)으로 오는지 자동으로 구분합니다.
        if (part2.more()) {
            auto r3 = socket.recv(part3, zmq::recv_flags::none);
            if (!r3) continue;
            // 3단 메시지인 경우
            header_msg = std::move(part2);
            payload_msg = std::move(part3);
        } else {
            // 2단 메시지인 경우
            header_msg = std::move(part1);
            payload_msg = std::move(part2);
        }

        if (header_msg.size() != sizeof(ViewerPacketHeader)) {
            // qDebug() << "[CAM" << m_cameraId << "] 헤더 크기 에러";
            continue; 
        }

        // 3. 정상 수신 처리 및 디코딩
        auto* header = static_cast<ViewerPacketHeader*>(header_msg.data());
        Q_UNUSED(header);
        QByteArray frameData(static_cast<const char*>(payload_msg.data()), static_cast<int>(payload_msg.size()));
        
        QImage frame;
        if (frame.loadFromData(frameData, "JPEG")) {
            // 구조체에 담긴 아이디를 무시하고, 이 포트에 할당된 m_cameraId를 기준으로 전송 (안전성 보장)
            emit frameReceived(m_cameraId, frame); 
        }
    } 
    
    qDebug() << QString("[VideoManager] CAM %1 ZMQ 스레드 정상 종료.").arg(m_cameraId);
}

void ZmqReceiverWorker::stopReceiving() {
    m_running = false;
}

// =========================================================
// VideoManager (멀티 스레드 제어부)
// =========================================================
VideoManager::VideoManager(const QString& controlEndpoint, QObject* parent) 
    : QObject(parent), m_controlEndpoint(controlEndpoint) 
{
    // 👉 카메라 대수 설정 (필요시 이 숫자를 8이나 16으로 늘리면 포트가 자동 할당됩니다)
    const int MAX_CAMERAS = 4;
    const int BASE_PORT = 7000;

    for (int i = 0; i < MAX_CAMERAS; ++i) {
        QThread* thread = new QThread(this);
        
        // tcp://10.10.10.100:7000, 7001, 7002... 순으로 생성
        QString endpoint = QString("tcp://10.10.10.100:%1").arg(BASE_PORT + i);
        ZmqReceiverWorker* worker = new ZmqReceiverWorker(i, endpoint);
        
        worker->moveToThread(thread);

        connect(thread, &QThread::started, worker, &ZmqReceiverWorker::startReceiving);
        connect(worker, &ZmqReceiverWorker::frameReceived, this, &VideoManager::newFrame, Qt::QueuedConnection);
        connect(worker, &ZmqReceiverWorker::logReady, this, &VideoManager::logReady);

        m_workerThreads.append(thread);
        m_workers.append(worker);
    }
}

VideoManager::~VideoManager() {
    stopStreaming();
    for (QThread* thread : m_workerThreads) {
        thread->quit();
        thread->wait();
    }
}

void VideoManager::startStreaming() {
    for (QThread* thread : m_workerThreads) {
        if (!thread->isRunning()) {
            thread->start();
        }
    }
}

void VideoManager::stopStreaming() {
    for (ZmqReceiverWorker* worker : m_workers) {
        QMetaObject::invokeMethod(worker, "stopReceiving", Qt::QueuedConnection);
    }
}

void VideoManager::sendControlCommand(int cameraId, int commandType, float /*value*/) {
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::req);
    
    socket.set(zmq::sockopt::rcvtimeo, 2000); 
    socket.set(zmq::sockopt::sndtimeo, 2000);
    
    try {
        socket.connect("tcp://10.10.10.100:9000");

        QJsonObject jsonObj;
        if (commandType == 1) {
            jsonObj["command"] = "camera_on";
        } else if (commandType == 2) {
            jsonObj["command"] = "camera_off";
        }
        jsonObj["cam_id"] = cameraId;
        
        QJsonDocument doc(jsonObj);
        QByteArray jsonBytes = doc.toJson(QJsonDocument::Compact);

        zmq::message_t request(jsonBytes.data(), jsonBytes.size());
        auto res = socket.send(request, zmq::send_flags::none);

        if (res) {
            zmq::message_t reply;
            auto rcvRes = socket.recv(reply, zmq::recv_flags::none);
            if (rcvRes) {
                QString replyStr = QString::fromUtf8(static_cast<char*>(reply.data()), reply.size());
                emit logReady(QString("[Control] CAM %1 제어 성공: %2").arg(cameraId).arg(replyStr));
            }
        }
    } catch (const zmq::error_t& e) {
        qDebug() << "[Control] ZMQ 통신 에러:" << e.what();
    }
}