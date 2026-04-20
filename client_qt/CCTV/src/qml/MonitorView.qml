import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: monitorView

    // 화면 상태 관리 변수
    property bool isSingleView: false
    property int expandedIndex: 0

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
                    background: Rectangle { color: "#2C75FF"; radius: 5; implicitWidth: 80; implicitHeight: 30 }
                }
                Button {
                    text: "설정"
                    contentItem: Text { text: parent.text; color: "#8A94A6"; font.pixelSize: 14; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "transparent" }
                    // 👉 수정: 설정 화면으로 이동 (오른쪽에서 왼쪽으로 슬라이드)
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
        // 2. 컨트롤 바 (중단 버튼)
        // ==========================================
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 60
            color: "#0B111E"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20; anchors.rightMargin: 20

                Text { text: "실시간 모니터링   <font color='#00FF00'>•</font> 실시간 송출 중"; color: "white"; font.pixelSize: 14; font.bold: true; textFormat: Text.RichText }
                
                Item { implicitWidth: 30 }

                Button {
                    text: "1개 화면"
                    contentItem: Text { text: parent.text; color: isSingleView ? "white" : "#8A94A6"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: isSingleView ? "#2C75FF" : "#1C2434"; radius: 5; implicitWidth: 80; implicitHeight: 30 }
                    onClicked: isSingleView = true
                }
                Button {
                    text: "4분할 화면"
                    contentItem: Text { text: parent.text; color: !isSingleView ? "white" : "#8A94A6"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: !isSingleView ? "#2C75FF" : "#1C2434"; radius: 5; implicitWidth: 80; implicitHeight: 30 }
                    onClicked: isSingleView = false
                }
                
                Item { Layout.fillWidth: true }

                Button {
                    text: "실시간"
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#2C75FF"; radius: 5; implicitWidth: 80; implicitHeight: 30 }
                }
                Button {
                    text: "이벤트 영상"
                    contentItem: Text { text: parent.text; color: "#8A94A6"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5; implicitWidth: 80; implicitHeight: 30 }
                    onClicked: stackView.push("PlaybackView.qml")
                }
            }
        }
// ==========================================
        // 3. 비디오 격자 및 하단 채널 바 (통합 영역)
        // ==========================================
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // 비디오 격자 영역
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                GridLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    columns: 2
                    rows: 2
                    columnSpacing: 15
                    rowSpacing: 15

                    Repeater {
                        model: 4
                        Rectangle {
                            id: videoFrame
                            Layout.columnSpan: (isSingleView && expandedIndex === index) ? 2 : 1
                            Layout.rowSpan: (isSingleView && expandedIndex === index) ? 2 : 1
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: !isSingleView || expandedIndex === index
                            color: "#141B2D"
                            radius: 8
                            border.color: (isSingleView && expandedIndex === index) ? "#2C75FF" : "#1C2434"
                            border.width: 1
                            property int frameCounter: 0

                            Connections {
                                target: sysController
                                function onFrameUpdated(camId) {
                                    if (camId === index) {
                                        videoFrame.frameCounter++
                                    }
                                }
                            }

                            RowLayout {
                                anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                                anchors.margins: 10
                                Text { text: "🔴 REC - 채널 " + (index + 1); color: "red"; font.bold: true; font.pixelSize: 12 }
                                Item { Layout.fillWidth: true }
                                Text { text: Qt.formatDateTime(new Date(), "ap hh:mm:ss"); color: "white"; font.pixelSize: 12 }
                            }

                            Image {
                                anchors.fill: parent
                                anchors.margins: 1 // 테두리를 덮지 않도록
                                fillMode: Image.PreserveAspectFit // 비율 유지
                                asynchronous: false
                                cache: false // 실시간 스트리밍이므로 캐싱 끄기
                                
                                // C++ 프로바이더 호출 ("image://cctv/채널번호")
                                // 뒤에 frameCounter를 붙여야 QML이 이미지가 바뀐 줄 알고 다시 요청합니다.
                                source: "image://cctv/" + index + "?id=" + videoFrame.frameCounter
                            }

                            MouseArea {
                                anchors.fill: parent
                                onDoubleClicked: {
                                    if (isSingleView) {
                                        isSingleView = false
                                    } else {
                                        expandedIndex = index
                                        isSingleView = true
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ==========================================
            // 4. 1개 화면 모드 전용 탐색 게이지 및 컨트롤러
            // ==========================================
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 90 // 버튼을 추가하기 위해 높이를 40에서 90으로 확장
                color: "#0B111E"
                visible: isSingleView

                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 25; anchors.rightMargin: 25; anchors.topMargin: 5; anchors.bottomMargin: 5
                    spacing: 0

                    // 4-1. 타임라인 슬라이더 영역
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 15
                        
                        Text { 
                            text: "LIVE"
                            color: "#FF0000"
                            font.pixelSize: 10
                            font.bold: true 
                        }

                        Slider {
                            id: liveSeekSlider
                            Layout.fillWidth: true
                            from: 0
                            to: 100
                            value: 100 // 기본적으로 가장 최근(오른쪽)에 위치
                            
                            background: Rectangle {
                                implicitHeight: 4; radius: 2; color: "#263043"
                                Rectangle {
                                    width: liveSeekSlider.visualPosition * parent.width
                                    height: parent.height; color: "#2C75FF"; radius: 2
                                }
                            }

                            handle: Rectangle {
                                x: liveSeekSlider.leftPadding + liveSeekSlider.visualPosition * (liveSeekSlider.availableWidth - width)
                                y: liveSeekSlider.topPadding + liveSeekSlider.availableHeight / 2 - height / 2
                                implicitWidth: 12; implicitHeight: 12; radius: 6; color: liveSeekSlider.pressed ? "#1A5BE6" : "#2C75FF"
                            }

                            onMoved: sysController.seekVideo(expandedIndex, value)
                        }
                        
                        Text { 
                            text: "0s"
                            color: "#8A94A6"
                            font.pixelSize: 11 
                        }
                    }

                    // 4-2. 재생 컨트롤 버튼 (-10초, 일시정지, +10초)
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 30

                        // 10초 뒤로 가기
                        //Button {
                            //text: "⏪ 10s"
                            //contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                            //background: Rectangle { color: "transparent" }
                            //onClicked: {
                                // 슬라이더 값을 감소시키고 C++로 전송 (from 값 이하로 내려가지 않도록 Math.max 사용)
                                //liveSeekSlider.value = Math.max(liveSeekSlider.from, liveSeekSlider.value - 10)
                                //sysController.seekVideo(expandedIndex, liveSeekSlider.value)
                            //}
                        //}

                        // 재생 / 일시정지
                        Button {
                            id: livePlayBtn
                            property bool isPlaying: true // 실시간이므로 기본값은 재생 중
                            text: isPlaying ? "⏸" : "▶"
                            contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 16; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: "#2C75FF"; radius: 20; implicitWidth: 40; implicitHeight: 40 }
                            onClicked: isPlaying = !isPlaying
                        }

                        // 10초 앞으로 가기
                        //Button {
                            //text: "10s ⏩"
                            //contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                            //background: Rectangle { color: "transparent" }
                            //onClicked: {
                                // 슬라이더 값을 증가시키고 C++로 전송 (to 값 이상으로 올라가지 않도록 Math.min 사용)
                                //liveSeekSlider.value = Math.min(liveSeekSlider.to, liveSeekSlider.value + 10)
                                //sysController.seekVideo(expandedIndex, liveSeekSlider.value)
                            //}
                        //}
                    }
                }
            }

            // ==========================================
            // 5. 하단 채널 선택 바
            // ==========================================
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 80
                color: "#0B111E"
                visible: isSingleView

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 15
                    spacing: 10
                    Repeater {
                        // 👉 쌍따옴표 누락 에러 수정됨! ("CAM 3" -> "CAM 3")
                        model: ["CAM 1", "CAM 2", "CAM 3", "CAM 4"]
                        Button {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            contentItem: Text { 
                                text: "🎥  " + modelData
                                color: "white"; font.pixelSize: 13
                                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle { 
                                color: (expandedIndex === index) ? "#2C75FF" : "#1C2434"
                                radius: 6
                            }
                            onClicked: expandedIndex = index
                        }
                    }
                }
            }
        } 
    } 
}