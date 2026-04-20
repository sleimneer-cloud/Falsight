import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: playbackView

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ==========================================
        // 1. 상단 네비게이션 바 (헤더)
        // ==========================================
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 60
            color: "#141B2D"
            Rectangle { width: parent.width; height: 1; color: "#1C2434"; anchors.bottom: parent.bottom }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20; anchors.rightMargin: 20

                Text { text: "CCTV 모니터링 시스템"; color: "white"; font.pixelSize: 16; font.bold: true }
                
                Item { Layout.fillWidth: true }

                Button {
                    text: "모니터링"
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 14; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "transparent" }
                    // 👉 수정: pop()을 사용하여 왼쪽에서 오른쪽으로 밀리며 복귀
                    onClicked: stackView.pop() 
                }
                Button {
                    text: "설정"
                    contentItem: Text { text: parent.text; color: "#8A94A6"; font.pixelSize: 14; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "transparent" }
                    // 👉 수정: 설정 화면으로 이동
                    onClicked: stackView.push("SettingsView.qml")
                }
                Button {
                    text: "로그아웃"
                    contentItem: Text { text: parent.text; color: "#8A94A6"; font.pixelSize: 14; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "transparent" }
                    onClicked: stackView.replace("LoginView.qml")
                }
            }
        }

        // ==========================================
        // 2. 컨트롤 바 (과거 영상 재생 옵션들)
        // ==========================================
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 60
            color: "#0B111E"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20; anchors.rightMargin: 20
                spacing: 15

                Text { text: "과거 영상 재생"; color: "white"; font.pixelSize: 15; font.bold: true }
                
                Item { implicitWidth: 20 }

                ComboBox {
                    model: ["CAM 1", "CAM 2", "CAM 3", "CAM 4"]
                    implicitWidth: 120
                    contentItem: Text { text: parent.displayText; color: "white"; leftPadding: 10; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5 }
                }

                Button {
                    text: "📅 2026. 04. 16."
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5; implicitWidth: 130; implicitHeight: 30 }
                }

                Button {
                    text: "🕒 오전 12:00"
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5; implicitWidth: 110; implicitHeight: 30 }
                }

                ComboBox {
                    model: ["1x", "2x", "4x", "8x"]
                    implicitWidth: 70
                    contentItem: Text { text: parent.displayText; color: "white"; leftPadding: 10; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5 }
                }

                Item { Layout.fillWidth: true }

                // 실시간 / 과거영상 토글 버튼
                Button {
                    text: "실시간"
                    contentItem: Text { text: parent.text; color: "#8A94A6"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5; implicitWidth: 80; implicitHeight: 30 }
                    // 👉 수정: pop()을 사용하면 화면이 오른쪽으로 밀려나가며 실시간 화면이 나타남
                    onClicked: stackView.pop() 
                }
                Button {
                    text: "과거 영상"
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#2C75FF"; radius: 5; implicitWidth: 80; implicitHeight: 30 }
                }
            }
        }

        // ==========================================
        // 3. 메인 영상 플레이어 영역
        // ==========================================
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 20
            color: "#141B2D"
            radius: 8
            border.color: "#1C2434"
            border.width: 1

            Text {
                anchors.centerIn: parent
                text: "▶️ 비디오 재생 영역"
                color: "#4A5568"
                font.pixelSize: 20
            }

            Text {
                anchors.top: parent.top; anchors.right: parent.right
                anchors.margins: 15
                text: "2026-04-16 00:00:00"
                color: "white"
                font.pixelSize: 13
                font.family: "Courier"
            }
        }

        // ==========================================
        // 4. 하단 타임라인 컨트롤러
        // ==========================================
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 120 // 버튼들을 포함하기 위해 높이 상향
            color: "#0B111E"
            Layout.leftMargin: 20; Layout.rightMargin: 20; Layout.bottomMargin: 20

            ColumnLayout {
                anchors.fill: parent
                spacing: 10

                // 타임라인 슬라이더
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "00:00:00"; color: "#8A94A6"; font.pixelSize: 12; font.family: "Courier" }
                    
                    Slider {
                        id: timeSlider
                        Layout.fillWidth: true
                        value: 0.0
                        
                        // 1. 배경 디자인
                        background: Rectangle {
                            x: timeSlider.leftPadding
                            y: timeSlider.topPadding + timeSlider.availableHeight / 2 - height / 2
                            implicitWidth: 200
                            implicitHeight: 4
                            width: timeSlider.availableWidth
                            height: implicitHeight
                            radius: 2
                            color: "#1C2434"
                            Rectangle { 
                                width: timeSlider.visualPosition * parent.width
                                height: parent.height
                                color: "#2C75FF"
                                radius: 2 
                            }
                        }
                        
                        // 2. 핸들 디자인
                        handle: Rectangle {
                            x: timeSlider.leftPadding + timeSlider.visualPosition * (timeSlider.availableWidth - width)
                            y: timeSlider.topPadding + timeSlider.availableHeight / 2 - height / 2
                            implicitWidth: 14
                            implicitHeight: 14
                            radius: 7
                            color: timeSlider.pressed ? "#1A5BE6" : "#2C75FF"
                        }

                        onMoved: {
                            // 사용자가 핸들을 조작했을 때 C++ 백엔드에 위치를 보냅니다.
                            // value는 0.0에서 1.0 사이의 값입니다.
                            console.log("사용자 조작 - 탐색 위치: " + (value * 100).toFixed(2) + "%")
                            
                            // C++ SystemController의 함수를 호출하여 실제 영상 위치를 바꿉니다.
                            // expandedIndex는 현재 보고 있는 채널 번호입니다.
                            sysController.seekVideo(expandedIndex, value * 100)
                        }
                    }
                    Text { text: "01:00:00"; color: "#8A94A6"; font.pixelSize: 12; font.family: "Courier" }
                }

                // 재생 컨트롤 버튼
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 25

                    Button {
                        text: "⏮"
                        contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 22; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { color: "transparent" }
                    }
                    Button {
                        id: playBtn
                        property bool isPlaying: false
                        text: isPlaying ? "⏸" : "▶"
                        contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 24; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { color: "#2C75FF"; radius: 25; implicitWidth: 50; implicitHeight: 50 }
                        onClicked: isPlaying = !isPlaying
                    }
                    Button {
                        text: "⏭"
                        contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 22; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { color: "transparent" }
                    }
                }
            }
        }
    }
}