#include "videoimageprovider.h"

VideoImageProvider::VideoImageProvider() 
    : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage VideoImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    // id는 QML에서 요청한 식별자 (예: "0", "1", "2", "3")
    int camId = id.toInt();
    QImage img;
    
    {
        QMutexLocker locker(&m_mutex);
        img = m_images.value(camId, QImage(1920, 1080, QImage::Format_RGB32)); // 데이터가 없으면 빈 이미지 반환
    }

    if (size) *size = img.size();
    
    // requestedSize가 있으면 리사이즈 처리 (생략 가능)
    if (requestedSize.isValid() && !img.isNull()) {
        img = img.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    
    return img;
}

void VideoImageProvider::updateImage(int cameraId, const QImage &image) {
    QMutexLocker locker(&m_mutex);
    m_images[cameraId] = image;
}