#ifndef ARDUINOMANAGER_H
#define ARDUINOMANAGER_H

#include <QObject>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QString>

class ArduinoManager : public QObject {
    Q_OBJECT
public:
    explicit ArduinoManager(QObject *parent = nullptr);
    ~ArduinoManager();

    // 시리얼 포트 연결 (리눅스는 보통 "/dev/ttyACM0" 또는 "/dev/ttyUSB0")
    bool connectDevice(const QString &portName, int baudRate = QSerialPort::Baud9600);
    void disconnectDevice();

public slots:
    // 알람 작동 명령 전송 ("ALARM_ON\n")
    void triggerAlarm();
    // 알람 해제 명령 전송 ("ALARM_OFF\n")
    void clearAlarm();

signals:
    void logReady(QString message); // UI 콘솔에 띄울 로그

private:
    QSerialPort *m_serial;
    void sendCommand(const QByteArray &command);
};

#endif // ARDUINOMANAGER_H