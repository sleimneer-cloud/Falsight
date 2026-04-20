import QtQuick
import QtQuick.Controls
import QtQuick.Window

Window {
    id: mainWindow
    width: 1280
    height: 720
    visible: true
    title: "낙상 감지 모니터링 시스템"
    color: "#0B111E" // 전체 다크 배경

    // 화면(페이지)을 쌓아서 전환할 수 있게 해주는 StackView
    StackView {
        id: stackView
        anchors.fill: parent
        initialItem: "LoginView.qml" // 처음 켜졌을 때 띄울 화면
    }
}