#ifndef VIDEOIMAGEPROVIDER_H
#define VIDEOIMAGEPROVIDER_H

#include <QQuickImageProvider>
#include <QImage>
#include <QMap>
#include <QMutex>

class VideoImageProvider : public QQuickImageProvider {
public:
    VideoImageProvider();

    // QML에서 이미지를 요청할 때 자동으로 호출되는 함수
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

    // C++ 백엔드(ZMQ)에서 새로운 프레임이 수신되었을 때 이미지를 갱신하는 함수
    void updateImage(int cameraId, const QImage &image);

private:
    QMap<int, QImage> m_images; // 채널별 최신 프레임 저장소
    QMutex m_mutex;             // 스레드 안전성 보장 (QML 렌더링 스레드 vs ZMQ 수신 스레드)
};

#endif // VIDEOIMAGEPROVIDER_H