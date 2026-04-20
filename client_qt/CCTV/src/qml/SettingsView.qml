import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: settingsView

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // 헤더 (공통)
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 60; color: "#141B2D"
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 20; anchors.rightMargin: 20
                Text { text: "CCTV 모니터링 시스템"; color: "white"; font.pixelSize: 16; font.bold: true }
                Item { Layout.fillWidth: true }
                Button { 
                    text: "모니터링"; background: Rectangle { color: "transparent" }
                    contentItem: Text { text: parent.text; color: "white" }
                    onClicked: stackView.replace("MonitorView.qml")
                }
                Button { 
                    text: "설정"; background: Rectangle { color: "#2C75FF"; radius: 5; implicitWidth: 80; implicitHeight: 30 }
                    contentItem: Text { text: parent.text; color: "white" }
                }
                Button { 
                    text: "로그아웃"; background: Rectangle { color: "transparent" }
                    contentItem: Text { text: parent.text; color: "white" }
                    onClicked: stackView.replace("LoginView.qml")
                }
            }
        }

        // 메인 설정 영역
        ScrollView {
            Layout.fillWidth: true; Layout.fillHeight: true
            contentWidth: availableWidth

            ColumnLayout {
                width: 500; anchors.horizontalCenter: parent.horizontalCenter
                spacing: 25; Layout.topMargin: 40; Layout.bottomMargin: 40

                ColumnLayout {
                    spacing: 8
                    Text { text: "계정 설정"; color: "white"; font.pixelSize: 20; font.bold: true }
                    Text { text: "아이디와 비밀번호를 변경할 수 있습니다."; color: "#8A94A6"; font.pixelSize: 13 }
                }

                // 섹션 1: 현재 정보
                ColumnLayout {
                    spacing: 15; Layout.fillWidth: true
                    Text { text: "현재 계정 정보 확인"; color: "white"; font.bold: true }
                    
                    ColumnLayout {
                        spacing: 5; Layout.fillWidth: true
                        Text { text: "현재 아이디"; color: "#8A94A6"; font.pixelSize: 12 }
                        TextField { 
                            Layout.fillWidth: true; placeholderText: "현재 아이디를 입력하세요"
                            color: "white"; background: Rectangle { color: "#1C2434"; radius: 6; border.color: "#263043" }
                        }
                    }
                    ColumnLayout {
                        spacing: 5; Layout.fillWidth: true
                        Text { text: "현재 비밀번호"; color: "#8A94A6"; font.pixelSize: 12 }
                        TextField { 
                            Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: "현재 비밀번호를 입력하세요"
                            color: "white"; background: Rectangle { color: "#1C2434"; radius: 6; border.color: "#263043" }
                        }
                    }
                }

                // 섹션 2: 새 정보
                ColumnLayout {
                    spacing: 15; Layout.fillWidth: true
                    Text { text: "새 계정 정보"; color: "white"; font.bold: true }
                    
                    ColumnLayout {
                        spacing: 5; Layout.fillWidth: true
                        Text { text: "새 아이디"; color: "#8A94A6"; font.pixelSize: 12 }
                        TextField { Layout.fillWidth: true; placeholderText: "새 아이디를 입력하세요"; color: "white"; background: Rectangle { color: "#1C2434"; radius: 6; border.color: "#263043" } }
                    }
                    ColumnLayout {
                        spacing: 5; Layout.fillWidth: true
                        Text { text: "새 비밀번호"; color: "#8A94A6"; font.pixelSize: 12 }
                        TextField { Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: "새 비밀번호를 입력하세요"; color: "white"; background: Rectangle { color: "#1C2434"; radius: 6; border.color: "#263043" } }
                    }
                    ColumnLayout {
                        spacing: 5; Layout.fillWidth: true
                        Text { text: "새 비밀번호 확인"; color: "#8A94A6"; font.pixelSize: 12 }
                        TextField { Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: "새 비밀번호를 다시 입력하세요"; color: "white"; background: Rectangle { color: "#1C2434"; radius: 6; border.color: "#263043" } }
                    }
                }

                Button {
                    Layout.fillWidth: true; Layout.topMargin: 10
                    contentItem: Text { text: "변경 저장"; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#2C75FF"; radius: 6; implicitHeight: 45 }
                }
            }
        }
    }
}