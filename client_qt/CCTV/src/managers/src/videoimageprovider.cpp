#include "videoimageprovider.h"
#include <QDebug>
#include <QStringList> // 👉 split 기능을 위해 추가 (보통 포함되어 있지만 확실히 하기 위해)

VideoImageProvider::VideoImageProvider() 
    : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage VideoImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    
    // 💡 [핵심 수정] "1?id=45" 형태로 들어오므로 '?' 기준으로 잘라서 앞부분만 가져옵니다.
    QString camIdStr = id.split('?').first();
    int camId = camIdStr.toInt();
    
    // 로그로 실제 분리된 ID 확인 (디버깅용)
    // qDebug() << "원본 id:" << id << "-> 파싱된 camId:" << camId; 
    
    QImage img;
    {
        QMutexLocker locker(&m_mutex);
        img = m_images.value(camId, QImage(1920, 1080, QImage::Format_RGB32)); 
    }

    if (size) *size = img.size();
    
    if (requestedSize.isValid() && !img.isNull()) {
        img = img.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    
    return img;
}

void VideoImageProvider::updateImage(int cameraId, const QImage &image) {
    QMutexLocker locker(&m_mutex);
    m_images[cameraId] = image;
}