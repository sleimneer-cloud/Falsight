#include "systemcontroller.h"
#include "videomanager.h"
#include "videoimageprovider.h"
#include "databasemanager.h"
#include "arduinomanager.h"
#include "mainservertcpclient.h"
#include <QImage> 
#include <QDebug>
#include <QTime>
#include <QJsonDocument> 
#include <QJsonObject>

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QUrl>
#include <QNetworkRequest>

SystemController::SystemController(QObject *parent) : QObject(parent)
{
    m_dbManager = new DatabaseManager(this);
    m_dbManager->initDatabase();
    m_videoManager = new VideoManager("tcp://10.10.10.100:9000", this);
    m_imageProvider = new VideoImageProvider();
    m_webSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    
    connect(m_videoManager, &VideoManager::newFrame, this, &SystemController::onNewFrameReceived);
    connect(m_videoManager, &VideoManager::logReady, this, &SystemController::onLogReady);
    m_videoManager->startStreaming();

    m_arduinoManager = new ArduinoManager(this);
    connect(m_arduinoManager, &ArduinoManager::logReady, this, &SystemController::onLogReady);
    m_arduinoManager->connectDevice("/dev/ttyACM0", QSerialPort::Baud9600);

    // 1. WebSocket 알람 클라이언트 설정 및 연결

    m_webSocket = new QWebSocket();
    connect(m_webSocket, &QWebSocket::connected, this, &SystemController::onWsConnected);
    connect(m_webSocket, &QWebSocket::disconnected, this, &SystemController::onWsDisconnected);
    connect(m_webSocket, &QWebSocket::textMessageReceived, this, &SystemController::onWsTextMessageReceived);

    QUrl url("ws://10.10.10.113:8000/ws/client/alerts");
    m_webSocket->open(url);

    // 2. 👉 메인 서버 TCP 클라이언트 설정 및 연결 (8100 포트)

    m_tcpClient = new MainServerTcpClient(this);
    connect(m_tcpClient, &MainServerTcpClient::logReady, this, &SystemController::onLogReady);
    
    // TCP 수신 완료 시그널 -> QML로 바이패스
    connect(m_tcpClient, &MainServerTcpClient::eventListReceived, this, [this](QVariantList list){
        emit eventListUpdated(list);
    });
    connect(m_tcpClient, &MainServerTcpClient::videoUrlReceived, this, [this](QString url){
        emit videoUrlReady(url);
    });

    m_tcpClient->connectToServer("10.10.10.113", 8100);

    m_netManager = new QNetworkAccessManager(this);
    connect(m_netManager, &QNetworkAccessManager::finished, this, &SystemController::onDownloadFinished);
}

SystemController::~SystemController()
{
    if (m_webSocket) {
        m_webSocket->close();
        m_webSocket->deleteLater();
    }
}

void SystemController::fetchEventList(int camId, qint64 startTime, qint64 endTime)
{
    if (m_tcpClient) {
        m_tcpClient->requestEventList(camId, startTime, endTime);
    }
}

void SystemController::fetchVideoUrl(int camId, qint64 eventId)
{
    if (m_tcpClient) {
        m_tcpClient->requestVideoUrl(camId, eventId);
    }
}

void SystemController::requestLogin(const QString &id, const QString &pw)
{
    if (m_dbManager->verifyLogin(id, pw)) {
        emit loginResult(true, "로그인 성공");
    } else {
        emit loginResult(false, "아이디 또는 비밀번호가 일치하지 않습니다.");
    }
}

void SystemController::seekVideo(int chIndex, float position) {
    qDebug() << "채널" << chIndex << "탐색 요청:" << position << "%";
    // commandType 1을 임의로 '탐색(Seek)' 명령이라 가정
    m_videoManager->sendControlCommand(chIndex, 1, position);
}

// ZMQ 워커 -> Manager -> Controller로 데이터가 도착함
void SystemController::onNewFrameReceived(int cameraId, QByteArray imageData) {
    QImage frame;
    // JPEG QByteArray를 QImage로 디코딩
    if (frame.loadFromData(imageData, "JPEG")) {
        // 프로바이더에 최신 이미지 업데이트
        m_imageProvider->updateImage(cameraId, frame);
        // QML에 해당 채널이 업데이트되었음을 알림
        emit frameUpdated(cameraId);
    } else {
        qDebug() << "JPEG 디코딩 실패! 채널:" << cameraId;
    }
}

void SystemController::onLogReady(QString message) {
    QString timeStr = QTime::currentTime().toString("hh:mm:ss");
    emit systemLog(timeStr, message);
}

// ==========================================================
// 👉 웹소켓 슬롯 구현부
// ==========================================================
void SystemController::onWsConnected() {
    qDebug() << "[WebSocket] 메인 서버 알람 채널 연결 성공!";
    onLogReady("[WebSocket] 메인 서버 알람 채널 연결 성공!");
}

void SystemController::onWsDisconnected() {
    qDebug() << "[WebSocket] 알람 채널 연결이 끊어졌습니다.";
    onLogReady("[WebSocket] 알람 채널 연결이 끊어졌습니다.");
}

void SystemController::onWsTextMessageReceived(QString message) {
    qDebug() << "[WebSocket] 수신된 메시지:" << message;

    // 1. 수신된 텍스트를 JSON으로 파싱
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (doc.isNull() || !doc.isObject()) {
        qDebug() << "잘못된 JSON 형식입니다.";
        return;
    }

    QJsonObject jsonObj = doc.object();

    // 2. 명세서에 따라 "event" 값이 "FALL"인지 확인
    if (jsonObj.contains("event") && jsonObj["event"].toString() == "FALL") {
        int camId = jsonObj["cam_id"].toInt();
        int fallId = jsonObj["fall_id"].toInt();

        // UI에 표시할 로그 생성
        QString logMsg = QString("[🚨 긴급] 낙상 감지! (카메라: %1, 사고번호: %2)").arg(camId).arg(fallId);
        onLogReady(logMsg);

        // 3. ⭐️ 아두이노 알람 작동 ⭐️
        if (m_arduinoManager) {
            m_arduinoManager->triggerAlarm();
        }
    }
}

// ==========================================================
// 👉 비디오 다운로드 요청 (QML에서 호출)
// ==========================================================
void SystemController::downloadVideo(const QString &urlStr, const QString &fileName)
{
    QUrl url(urlStr);
    if (!url.isValid()) {
        onLogReady("[다운로드 에러] 유효하지 않은 URL입니다.");
        return;
    }

    // OS의 기본 '다운로드' 폴더 내에 'CCTV_Events' 폴더 생성
    QString savePath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/CCTV_Events";
    QDir dir(savePath);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    QString fullFilePath = savePath + "/" + fileName;

    QNetworkRequest request(url);
    QNetworkReply *reply = m_netManager->get(request);
    
    // 💡 최적화: 메모리에 쌓아두지 않고, 데이터를 받는 즉시 파일에 쓰도록 QFile 생성
    QFile *file = new QFile(fullFilePath, reply); // reply가 파괴될 때 file도 같이 파괴되도록 부모 설정
    
    if (file->open(QIODevice::WriteOnly)) {
        // 데이터가 도착할 때마다 쪼개서 디스크에 저장 (RAM 점유율 최소화)
        connect(reply, &QNetworkReply::readyRead, this, [reply, file]() {
            file->write(reply->readAll());
        });
        
        reply->setProperty("saveFilePath", fullFilePath);
        onLogReady(QString("[다운로드 시작] %1 저장 중...").arg(fileName));
    } else {
        onLogReady("[다운로드 에러] 파일을 생성할 수 없습니다.");
        reply->deleteLater();
    }
}

// ==========================================================
// 👉 비디오 다운로드 완료 처리
// ==========================================================
void SystemController::onDownloadFinished(QNetworkReply *reply)
{
    if (reply->error() == QNetworkReply::NoError) {
        QString saveFilePath = reply->property("saveFilePath").toString();
        onLogReady(QString("[다운로드 완료] 성공적으로 저장되었습니다: %1").arg(saveFilePath));
    } else {
        onLogReady("[다운로드 에러] 통신 실패: " + reply->errorString());
    }
    
    // reply를 지우면, 아까 부모로 묶어둔 QFile도 자동으로 close() 되고 메모리 해제됨
    reply->deleteLater(); 
}