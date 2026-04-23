QT += core gui qml quick sql network serialport

CONFIG += c++17
# pkg-config를 사용하여 zmq 경로를 자동으로 잡도록 설정
CONFIG += link_pkgconfig
PKGCONFIG += libzmq

DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000

# ========================================================
# 💡 Include Path 설정 (경로 지옥 탈출)
# 이제 소스 코드 어디서든 폴더 경로 없이 파일명만 쓸 수 있습니다.
# ========================================================
INCLUDEPATH += $$PWD/src \
               $$PWD/src/managers/header \
               $$PWD/src/controllers/header

# ========================================================
# 소스 파일 (.cpp)
# ========================================================
SOURCES += \
    src/main.cpp \
    src/managers/src/databasemanager.cpp \
    src/managers/src/networkmanager.cpp \
    src/managers/src/videomanager.cpp \
    src/managers/src/videoimageprovider.cpp \
    src/controllers/src/systemcontroller.cpp

# ========================================================
# 헤더 파일 (.h)
# ========================================================
HEADERS += \
    src/managers/header/databasemanager.h \
    src/managers/header/networkmanager.h \
    src/managers/header/videomanager.h \
    src/managers/header/videoimageprovider.h \
    src/managers/header/packetheader.h \
    src/controllers/header/systemcontroller.h

# ========================================================
# 리소스 파일 (.qrc)
# ========================================================
RESOURCES += \
    src/resources.qrc