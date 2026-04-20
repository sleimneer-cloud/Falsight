// src/managers/src/mainservertcpclient.cpp
#include "mainservertcpclient.h"
#include "packetheader.h"
#include <QtEndian>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>

MainServerTcpClient::MainServerTcpClient(QObject *parent) : QObject(parent) {
    connect(&m_socket, &QTcpSocket::connected, this, &MainServerTcpClient::onConnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &MainServerTcpClient::onReadyRead);
    connect(&m_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred),
            this, &MainServerTcpClient::onError);
}

void MainServerTcpClient::connectToServer(const QString &ip, quint16 port) {
    emit logReady(QString("[MainServer TCP] 연결 시도 -> %1:%2").arg(ip).arg(port));
    m_socket.connectToHost(ip, port);
}

void MainServerTcpClient::onConnected() {
    emit logReady("[MainServer TCP] 메인 서버 접속 성공! (10.10.10.113:8100)");
}

// 1. [ID: 310] 리스트 요청
void MainServerTcpClient::requestEventList(int camId, qint64 startTime, qint64 endTime) {
    if (m_socket.state() != QTcpSocket::ConnectedState) {
        emit logReady("[MainServer TCP] 연결 안됨! 리스트 요청 불가.");
        return;
    }

    ReqEventListHeader req;
    req.message_id = qToBigEndian<uint16_t>(310);
    req.camera_id  = static_cast<uint8_t>(camId);
    req.reserved   = 0x00;
    req.start_time = qToBigEndian<uint64_t>(static_cast<uint64_t>(startTime));
    req.end_time   = qToBigEndian<uint64_t>(static_cast<uint64_t>(endTime));

    m_socket.write(reinterpret_cast<const char*>(&req), sizeof(req));
    emit logReady(QString("[ID:310] 이벤트 리스트 요청 전송 (CAM: %1)").arg(camId));
}

// 2. [ID: 302] 영상 URL 요청
void MainServerTcpClient::requestVideoUrl(int camId, qint64 eventId) {
    if (m_socket.state() != QTcpSocket::ConnectedState) return;

    ReqEventVideoHeader req;
    req.message_id = qToBigEndian<uint16_t>(302);
    req.camera_id  = static_cast<uint8_t>(camId);
    req.event_type = 0x01; // 낙상
    req.event_id   = qToBigEndian<uint64_t>(static_cast<uint64_t>(eventId));
    req.timestamp  = qToBigEndian<uint64_t>(QDateTime::currentMSecsSinceEpoch());

    m_socket.write(reinterpret_cast<const char*>(&req), sizeof(req));
    emit logReady(QString("[ID:302] 이벤트 영상 URL 요청 전송 (EventID: %1)").arg(eventId));
}

// 3. 비동기 수신 처리 (스트림 분절 해결)
void MainServerTcpClient::onReadyRead() {
    m_buffer.append(m_socket.readAll());

    while (true) {
        // [상태 1] 20바이트 헤더 읽기 대기
        if (!m_isReadingPayload) {
            if (m_buffer.size() < sizeof(ResMainServerHeader)) return; // 데이터 부족

            auto* header = reinterpret_cast<const ResMainServerHeader*>(m_buffer.constData());
            m_currentMsgId = qFromBigEndian<uint16_t>(header->message_id);
            m_currentStatusCode = header->status_code;
            m_expectedPayloadSize = qFromBigEndian<uint32_t>(header->payload_size);

            m_buffer.remove(0, sizeof(ResMainServerHeader));
            m_isReadingPayload = true;
        }

        // [상태 2] 페이로드(JSON/URL) 읽기 대기
        if (m_isReadingPayload) {
            if (m_buffer.size() < m_expectedPayloadSize) return; // 페이로드 덜 옴

            QByteArray payloadData = m_buffer.left(m_expectedPayloadSize);
            m_buffer.remove(0, m_expectedPayloadSize);
            m_isReadingPayload = false; // 초기화 (다음 패킷을 위해)

            // 에러 처리
            if (m_currentStatusCode != 0x00) {
                emit logReady(QString("[MainServer Error] 코드: %1, 사유: %2").arg(m_currentStatusCode).arg(QString::fromUtf8(payloadData)));
                continue;
            }

            // 응답 분기 처리
            if (m_currentMsgId == 311) {
                // 리스트 응답 파싱
                QJsonDocument doc = QJsonDocument::fromJson(payloadData);
                emit logReady("[ID:311] 이벤트 리스트 응답 수신 완료");
                if (doc.isArray()) {
                    emit eventListReceived(doc.array().toVariantList());
                }
            } else if (m_currentMsgId == 303) {
                // URL 응답 파싱 (단순 텍스트 스트링 또는 JSON)
                QString url = QString::fromUtf8(payloadData).trimmed();
                emit logReady("[ID:303] 스트리밍 URL 수신: " + url);
                emit videoUrlReceived(url);
            }
        }
    }
}

void MainServerTcpClient::onError(QAbstractSocket::SocketError socketError) {
    emit logReady("[MainServer TCP] 통신 에러: " + m_socket.errorString());
}