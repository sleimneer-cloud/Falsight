import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: monitorView

    // 화면 상태 관리 변수
    property bool isSingleView: false
    property int expandedIndex: 0

    // 👉 1. 토스트 알림창 데이터를 담을 모델 (대기열 역할 대체)
    ListModel { id: alertModel }

    // 👉 2. 백엔드 시그널 수신부
    Connections {
        target: sysController
        
        function onEmergencyFallDetected(camId, fallId) {
            // 중복 스팸 방지: 이미 떠있는 동일한 사고 번호면 무시
            for (var i = 0; i < alertModel.count; i++) {
                if (alertModel.get(i).fallId === fallId) return;
            }

            console.log("🚨 [토스트 알림 추가] CAM:", camId, " FallID:", fallId)
            
            // 모델에 새 알림 추가 (자동으로 우측 하단에 생성됨)
            alertModel.append({ "camId": camId, "fallId": fallId })
        }
    }

    // 👉 3. 특정 카메라가 현재 알림 상태인지 체크하는 함수 (테두리 색상용)
    function isCameraAlerting(cameraIndex) {
        for (var i = 0; i < alertModel.count; i++) {
            if (alertModel.get(i).camId === (cameraIndex + 1)) return true;
        }
        return false;
    }

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
                            
                            // 👉 4. 테두리 색상: 현재 토스트 알림 목록에 이 카메라가 있으면 빨간색!
                            border.color: {
                                if (isCameraAlerting(index)) {
                                    return "#FF4444"
                                } else if (isSingleView && expandedIndex === index) {
                                    return "#2C75FF"
                                } else {
                                    return "#1C2434"
                                }
                            }
                            border.width: isCameraAlerting(index) ? 3 : 1
                            
                            // 빨간 테두리 깜빡임 효과
                            SequentialAnimation on border.color {
                                loops: Animation.Infinite
                                running: isCameraAlerting(index)
                                ColorAnimation { to: "#FF4444"; duration: 400 }
                                ColorAnimation { to: "#8B0000"; duration: 400 }
                            }

                            property int frameCounter: 0

                            Connections {
                                target: sysController
                              
                                function onFrameUpdated(camId) {
                                     if (parseInt(camId) === parseInt(index)) {  
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
                                anchors.margins: 1 
                                fillMode: Image.PreserveAspectFit 
                                asynchronous: false
                                cache: false 
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
                implicitHeight: 90 
                color: "#0B111E"
                visible: isSingleView

                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 25; anchors.rightMargin: 25; anchors.topMargin: 5; anchors.bottomMargin: 5
                    spacing: 0

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
                            value: 100 
                            
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

                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 30

                        Button {
                            id: livePlayBtn
                            property bool isPlaying: true 
                            text: isPlaying ? "⏸" : "▶"
                            contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 16; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: "#2C75FF"; radius: 20; implicitWidth: 40; implicitHeight: 40 }
                            onClicked: isPlaying = !isPlaying
                        }
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

    // ==========================================
    // 🚨 6. 토스트 알림 리스트 뷰 (화면 우측 하단 고정)
    // ==========================================
    ListView {
        id: toastList
        width: 320
        height: parent.height - 100 // 상단 여백 확보
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 20
        spacing: 15
        
        // 데이터가 밑에서부터 위로 쌓이게 만듦
        verticalLayoutDirection: ListView.BottomToTop
        interactive: false // 스크롤 비활성화 (토스트는 고정)
        
        model: alertModel

        // 애니메이션 효과 (부드럽게 등장)
        add: Transition {
            NumberAnimation { property: "x"; from: 350; duration: 400; easing.type: Easing.OutBack }
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 400 }
        }
        remove: Transition {
            NumberAnimation { property: "opacity"; to: 0; duration: 300 }
        }

        delegate: Rectangle {
            width: toastList.width
            height: 120
            color: "#141B2D"
            border.color: "#FF4444"
            border.width: 2
            radius: 8

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 15
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "🚨 긴급 낙상 감지!"; color: "#FF4444"; font.pixelSize: 16; font.bold: true }
                    Item { Layout.fillWidth: true }
                    Text { text: "CAM " + model.camId; color: "white"; font.pixelSize: 16; font.bold: true }
                }
                
                Text { text: "사고 번호: #" + model.fallId; color: "#8A94A6"; font.pixelSize: 13 }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 5
                    spacing: 10

                    Button {
                        Layout.fillWidth: true
                        text: "닫기"
                        contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { color: "#4A5568"; radius: 6; implicitHeight: 30 }
                        onClicked: {
                            alertModel.remove(index) // 알림 카드 삭제
                            checkAndStopAlarm()
                        }
                    }

                    Button {
                        Layout.fillWidth: true
                        text: "영상 확인"
                        contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { color: "#FF4444"; radius: 6; implicitHeight: 30 }
                        onClicked: {
                            // 영상 재생 화면으로 이동하면서 데이터 전달
                            var selectedCam = model.camId
                            var selectedFall = model.fallId
                            
                            alertModel.remove(index) // 확인했으니 카드 삭제
                            checkAndStopAlarm()
                            
                            stackView.push("PlaybackView.qml", {
                                "autoPlayTargetCamId": selectedCam,
                                "autoPlayTargetFallId": selectedFall
                            })
                        }
                    }
                }
            }
        }
    }

    // 👉 7. 알림이 모두 지워졌는지 확인하고 아두이노 끄는 함수
    function checkAndStopAlarm() {
        if (alertModel.count === 0) {
            console.log("모든 알림 확인 완료 -> 아두이노 알람 해제")
            sysController.stopArduinoAlarm() // 백엔드 아두이노 끄기 함수 호출
        }
    }
}