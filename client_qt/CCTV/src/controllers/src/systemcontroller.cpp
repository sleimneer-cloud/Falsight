#include "systemcontroller.h"
#include "videomanager.h"
#include "videoimageprovider.h"
#include "databasemanager.h"
#include "arduinomanager.h"
#include <QImage> // 👉 incomplete type 에러 해결!
#include <QDebug>
#include <QTime>

SystemController::SystemController(QObject *parent) : QObject(parent)
{
    m_dbManager = new DatabaseManager(this);
    m_dbManager->initDatabase(); // 앱 시작 시 DB 초기화 (admin 계정 생성)
    m_videoManager = new VideoManager("tcp://10.10.10.100:9000", this);
    // 프로바이더 생성
    m_imageProvider = new VideoImageProvider();
    // ZMQ에서 프레임이 도착하면 Controller의 슬롯으로 연결
    connect(m_videoManager, &VideoManager::newFrame, this, &SystemController::onNewFrameReceived);
    connect(m_videoManager, &VideoManager::logReady, this, &SystemController::onLogReady);

    m_videoManager->startStreaming();

    // 👉 아두이노 매니저 생성 및 로그 연결
    m_arduinoManager = new ArduinoManager(this);
    connect(m_arduinoManager, &ArduinoManager::logReady, this, &SystemController::onLogReady);

    // 👉 리눅스(우분투) 환경 아두이노 포트 연결 (USB 포트에 꽂은 뒤 포트 확인 필요)
    // 보통 /dev/ttyACM0 또는 /dev/ttyUSB0 입니다.
    m_arduinoManager->connectDevice("/dev/ttyACM0", QSerialPort::Baud9600);
}

SystemController::~SystemController()
{
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