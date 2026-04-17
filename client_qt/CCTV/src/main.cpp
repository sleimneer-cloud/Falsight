#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "systemcontroller.h" // 경로 확인 필요
#include "videoimageprovider.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    // 1. C++ 백엔드 컨트롤러 인스턴스 생성
    SystemController sysController;

    // 2. QML 엔진 생성
    QQmlApplicationEngine engine;

    // 3. QML에서 사용할 수 있도록 C++ 객체 및 이미지 프로바이더 등록
    engine.rootContext()->setContextProperty("sysController", &sysController);
    engine.addImageProvider("cctv", sysController.getImageProvider());

    // 4. 메인 QML 파일 로드 (Qt6 권장 방식 하나만 남김)
    // const QUrl url(u"qrc:/qml/main.qml"_qs);
    const QUrl url("qrc:/qml/main.qml");
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
        &app, [url](QObject *obj, const QUrl &objUrl) {
            if (!obj && url == objUrl)
                QCoreApplication::exit(-1);
        }, Qt::QueuedConnection);
    engine.load(url);

    return app.exec();
}