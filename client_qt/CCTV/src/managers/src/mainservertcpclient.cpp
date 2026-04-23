#include "mainservertcpclient.h"
#include "packetheader.h"
#include <QtEndian>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>

MainServerTcpClient::MainServerTcpClient(QObject *parent) : QObject(parent) {
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, &MainServerTcpClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &MainServerTcpClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &MainServerTcpClient::onReadyRead);
}

MainServerTcpClient::~MainServerTcpClient() {
    m_socket->disconnectFromHost();
}

void MainServerTcpClient::connectToServer(const QString& ip, quint16 port) {
    emit logReady(QString("[TCP] 메인 서버(%1:%2) 연결 시도 중...").arg(ip).arg(port));
    m_socket->connectToHost(ip, port);
}

void MainServerTcpClient::onConnected() {
    emit logReady("[TCP] 8100번 메인 서버 제어 포트 연결 성공!");
}

void MainServerTcpClient::onDisconnected() {
    emit logReady("[TCP] 8100번 메인 서버와 연결이 끊어졌습니다.");
}

// ==============================================================================
// 1. 송신: 리스트 요청 (310)
// ==============================================================================
void MainServerTcpClient::requestEventList(int camId, qint64 startTime, qint64 endTime) {
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        emit logReady("[TCP 에러] 서버에 연결되어 있지 않습니다.");
        return;
    }

    ReqEventListHeader req;
    req.message_id = qToBigEndian<uint16_t>(310);
    req.camera_id  = static_cast<uint8_t>(camId);
    req.reserved   = 0;
    req.start_time = qToBigEndian<uint64_t>(static_cast<uint64_t>(startTime));
    req.end_time   = qToBigEndian<uint64_t>(static_cast<uint64_t>(endTime));

    m_socket->write(reinterpret_cast<const char*>(&req), sizeof(ReqEventListHeader));
    m_socket->flush();
}

// ==============================================================================
// 2. 송신: 영상 URL 요청 (302)
// ==============================================================================
void MainServerTcpClient::requestVideoUrl(int camId, qint64 eventId) {
    if (m_socket->state() != QAbstractSocket::ConnectedState) return;

    ReqEventVideoHeader req;
    req.message_id = qToBigEndian<uint16_t>(302);
    req.camera_id  = static_cast<uint8_t>(camId);
    req.event_type = 0x01; // 임의 지정 (명세서에 맞게)
    req.event_id   = qToBigEndian<uint64_t>(static_cast<uint64_t>(eventId));
    req.timestamp  = qToBigEndian<uint64_t>(static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()));

    m_socket->write(reinterpret_cast<const char*>(&req), sizeof(ReqEventVideoHeader));
    m_socket->flush();
}

// ==============================================================================
// 3. 수신: 데이터 파싱 (TCP 단편화 및 Big-Endian 처리)
// ==============================================================================
void MainServerTcpClient::onReadyRead() {
    m_buffer.append(m_socket->readAll());

    // 헤더(20바이트) 이상 데이터가 모였을 때만 처리 진행
    while (m_buffer.size() >= static_cast<int>(sizeof(ResMainServerHeader))) {
        
        // 1. 버퍼에서 헤더만 살짝 복사해서 읽기
        ResMainServerHeader header;
        memcpy(&header, m_buffer.constData(), sizeof(ResMainServerHeader));

        // 2. Big-Endian 숫자를 내 컴퓨터 숫자(Little-Endian)로 변환
        uint16_t msgId = qFromBigEndian<uint16_t>(header.message_id);
        uint32_t payloadSize = qFromBigEndian<uint32_t>(header.payload_size);

        // 3. 헤더 + 페이로드가 모두 도착했는지 검사 (아직 덜 왔으면 루프 탈출 후 대기)
        if (m_buffer.size() < static_cast<int>(sizeof(ResMainServerHeader) + payloadSize)) {
            break; 
        }

        // 4. 온전한 패킷이 다 모였으므로 페이로드 데이터 추출
        QByteArray payload = m_buffer.mid(sizeof(ResMainServerHeader), payloadSize);
        
        // 5. 처리한 만큼 버퍼에서 잘라내기
        m_buffer.remove(0, sizeof(ResMainServerHeader) + payloadSize);

        // ========================================================
        // 수신된 데이터 분기 처리
        // ========================================================
        if (header.status_code == 0x01) {
            emit logReady("[TCP] 응답 데이터가 없습니다 (status: 0x01).");
            if (msgId == 311) emit eventListReceived(QVariantList()); // 빈 리스트 넘김
            continue;
        }

        if (msgId == 311) {
            // JSON 배열 파싱 (이벤트 리스트)
            QJsonDocument doc = QJsonDocument::fromJson(payload);
            if (doc.isArray()) {
                QVariantList list = doc.array().toVariantList();
                emit logReady(QString("[TCP] 이벤트 리스트 %1건 수신 완료").arg(list.size()));
                emit eventListReceived(list);
            } else {
                emit logReady("[TCP 에러] 수신된 311 페이로드가 JSON 배열이 아닙니다.");
            }
        } 
        else if (msgId == 303) {
            // 일반 텍스트 파싱 (영상 URL)
            QString url = QString::fromUtf8(payload);
            emit logReady("[TCP] 영상 URL 수신 완료: " + url);
            emit videoUrlReceived(url);
        }
    }
}