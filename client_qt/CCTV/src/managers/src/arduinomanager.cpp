#include "arduinomanager.h"
#include <QDebug>

ArduinoManager::ArduinoManager(QObject *parent) : QObject(parent) {
    m_serial = new QSerialPort(this);
}

ArduinoManager::~ArduinoManager() {
    disconnectDevice();
}

bool ArduinoManager::connectDevice(const QString &portName, int baudRate) {
    if (m_serial->isOpen()) {
        m_serial->close();
    }

    m_serial->setPortName(portName);
    m_serial->setBaudRate(baudRate);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);

    if (m_serial->open(QIODevice::ReadWrite)) {
        emit logReady(QString("[Arduino] 아두이노 연결 성공 -> %1").arg(portName));
        return true;
    } else {
        emit logReady(QString("[Arduino] 아두이노 연결 실패! 포트: %1, 에러: %2")
                          .arg(portName, m_serial->errorString()));
        return false;
    }
}

void ArduinoManager::disconnectDevice() {
    if (m_serial->isOpen()) {
        m_serial->close();
        emit logReady("[Arduino] 아두이노 연결 해제됨");
    }
}

void ArduinoManager::triggerAlarm() {
    // 아두이노로 보낼 낙상 알람 작동 명령어
    sendCommand("ALARM_ON\n");
    emit logReady("[Arduino] 🚨 긴급 알람 명령 전송 완료 (ALARM_ON)");
}

void ArduinoManager::clearAlarm() {
    // 아두이노로 보낼 낙상 알람 해제 명령어
    sendCommand("ALARM_OFF\n");
    emit logReady("[Arduino] 🛑 알람 해제 명령 전송 완료 (ALARM_OFF)");
}

void ArduinoManager::sendCommand(const QByteArray &command) {
    if (m_serial->isOpen() && m_serial->isWritable()) {
        m_serial->write(command);
        m_serial->waitForBytesWritten(100);
    } else {
        emit logReady("[Arduino] Error: 포트가 닫혀있어 명령을 보낼 수 없습니다.");
    }
}

void ArduinoManager::stopAlarm() {
    sendCommand("ALARM_OFF\n");
}