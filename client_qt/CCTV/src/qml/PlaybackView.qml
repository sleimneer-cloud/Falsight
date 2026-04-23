import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia 

Item {
    id: playbackView

    property bool isEventListOpen: false 
    property string currentPlayUrl: ""
    property string currentFileName: "event_video.mp4"
    
    // 👉 수정됨: 단일 날짜 대신 시작 날짜와 종료 날짜 프로퍼티로 분리
    property date startDate: new Date(new Date().getFullYear(), new Date().getMonth(), new Date().getDate() - 7) // 기본값: 7일 전
    property date endDate: new Date() // 기본값: 오늘
    property bool isSelectingStart: true // 팝업이 열릴 때 시작일을 수정하는지, 종료일을 수정하는지 구분하는 플래그

    // ==========================================
    // C++ SystemController의 시그널 수신부
    // ==========================================
    Connections {
        target: sysController

        function onEventListUpdated(list) {
            eventListModel.clear() 
            for (var i = 0; i < list.length; i++) {
                var item = list[i];
                var date = new Date(item.timestamp);
                var timeStr = date.toLocaleTimeString(Qt.locale("ko_KR"), "ap hh:mm:ss");
                
                eventListModel.append({
                    "eventId": item.event_id,
                    "camId": item.camera_id,
                    "time": timeStr,
                    "status": "낙상 감지",
                    "cam": "CAM " + (item.camera_id)
                });
            }
        }

        function onVideoUrlReady(streamUrl) {
            console.log("스트리밍 URL 수신:", streamUrl)
            currentPlayUrl = streamUrl
            mediaPlayer.source = streamUrl
            mediaPlayer.play() 
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // 1. 상단 타이틀 바 (기존과 동일)
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
                    onClicked: stackView.pop() 
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
        // 2. 컨트롤 바 (날짜 범위 선택 & 리스트 요청)
        // ==========================================
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 60
            color: "#0B111E"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20; anchors.rightMargin: 20
                spacing: 15

                Text { text: "이벤트 영상"; color: "white"; font.pixelSize: 15; font.bold: true }
                Item { implicitWidth: 10 }
                
                ComboBox {
                    model: ["CAM 전체", "CAM 1", "CAM 2", "CAM 3", "CAM 4"] // 편의를 위해 전체 옵션 추가
                    implicitWidth: 110
                    contentItem: Text { text: parent.displayText; color: "white"; leftPadding: 10; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5 }
                }

                // 👉 수정됨: 시작 날짜 버튼
                Button {
                    id: startDateBtn
                    text: "📅 " + Qt.formatDate(startDate, "yyyy.MM.dd")
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5; implicitWidth: 110; implicitHeight: 30 }
                    onClicked: {
                        isSelectingStart = true
                        yearTumbler.currentIndex = startDate.getFullYear() - 2000
                        monthTumbler.currentIndex = startDate.getMonth()
                        dayTumbler.currentIndex = startDate.getDate() - 1
                        datePopup.open()
                    }
                }

                // 👉 수정됨: 물결(~) 텍스트 
                Text { text: "~"; color: "white"; font.bold: true; font.pixelSize: 16 }

                // 👉 수정됨: 종료 날짜 버튼
                Button {
                    id: endDateBtn
                    text: "📅 " + Qt.formatDate(endDate, "yyyy.MM.dd")
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5; implicitWidth: 110; implicitHeight: 30 }
                    onClicked: {
                        isSelectingStart = false
                        yearTumbler.currentIndex = endDate.getFullYear() - 2000
                        monthTumbler.currentIndex = endDate.getMonth()
                        dayTumbler.currentIndex = endDate.getDate() - 1
                        datePopup.open()
                    }
                }

                // 공용 날짜 선택 팝업
                Popup {
                    id: datePopup
                    y: startDateBtn.height + 5 
                    width: 240
                    height: 200
                    padding: 10
                    background: Rectangle { color: "#141B2D"; radius: 8; border.color: "#2C75FF"; border.width: 1 }

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 10
                        
                        // 현재 어떤 날짜를 고르고 있는지 헤더 텍스트
                        Text { 
                            text: isSelectingStart ? "시작 날짜 선택" : "종료 날짜 선택" 
                            color: "#2C75FF"
                            font.bold: true
                            Layout.alignment: Qt.AlignHCenter
                        }
                        
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            
                            Tumbler {
                                id: yearTumbler; model: 100 // 2000~2099
                                delegate: Text {
                                    text: (2000 + index) + "년"
                                    color: Tumbler.displacement === 0 ? "white" : "#8A94A6"
                                    font.pixelSize: Tumbler.displacement === 0 ? 15 : 12
                                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                }
                            }
                            Tumbler {
                                id: monthTumbler; model: 12
                                delegate: Text {
                                    text: (index + 1) + "월"
                                    color: Tumbler.displacement === 0 ? "white" : "#8A94A6"
                                    font.pixelSize: Tumbler.displacement === 0 ? 15 : 12
                                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                }
                            }
                            Tumbler {
                                id: dayTumbler; model: 31
                                delegate: Text {
                                    text: (index + 1) + "일"
                                    color: Tumbler.displacement === 0 ? "white" : "#8A94A6"
                                    font.pixelSize: Tumbler.displacement === 0 ? 15 : 12
                                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                }
                            }
                        }

                        RowLayout {
                            Layout.alignment: Qt.AlignRight
                            spacing: 10
                            
                            Button {
                                text: "취소"
                                contentItem: Text { text: parent.text; color: "#8A94A6"; font.pixelSize: 12 }
                                background: Rectangle { color: "transparent" }
                                onClicked: datePopup.close()
                            }
                            
                            Button {
                                text: "적용"
                                contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 12; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                                background: Rectangle { color: "#2C75FF"; radius: 4; implicitWidth: 50; implicitHeight: 25 }
                                onClicked: {
                                    var y = 2000 + yearTumbler.currentIndex
                                    var m = monthTumbler.currentIndex
                                    var d = dayTumbler.currentIndex + 1
                                    
                                    // 👉 수정됨: 선택한 날짜를 변수에만 반영하고 통신은 이벤트 목록 버튼에서 수행
                                    if (isSelectingStart) {
                                        startDate = new Date(y, m, d)
                                    } else {
                                        endDate = new Date(y, m, d)
                                    }
                                    datePopup.close()
                                }
                            }
                        }
                    }
                }

                // 👉 수정됨: 이벤트 목록 조회 버튼 (여기서 통신 요청 실행)
                Button {
                    text: isEventListOpen ? "목록 닫기 ❌" : "조회 요청 📋"
                    contentItem: Text { text: parent.text; color: isEventListOpen ? "#FF4444" : "#00FF00"; font.pixelSize: 13; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5; implicitWidth: 110; implicitHeight: 30; border.color: isEventListOpen ? "#FF4444" : "#00FF00" }
                    onClicked: {
                        isEventListOpen = !isEventListOpen
                        if (isEventListOpen) {
                            // 시작일의 00:00:00 Unix Timestamp (ms)
                            var startOfDay = new Date(startDate.getFullYear(), startDate.getMonth(), startDate.getDate(), 0, 0, 0).getTime()
                            
                            // 종료일의 23:59:59 Unix Timestamp (ms)
                            var endOfDay = new Date(endDate.getFullYear(), endDate.getMonth(), endDate.getDate(), 23, 59, 59).getTime()
                            
                            console.log("메인서버(8100/310번)로 기간 데이터 요청 -> 시작:", Qt.formatDate(startDate, "yyyy-MM-dd"), "종료:", Qt.formatDate(endDate, "yyyy-MM-dd"))
                            
                            // C++의 fetchEventList 호출 (TCP 패킷 310번으로 전송됨)
                            sysController.fetchEventList(255, startOfDay, endOfDay)
                        }
                    }
                }

                Item { Layout.fillWidth: true } // 우측 정렬용 스페이서

                Button {
                    text: "실시간"
                    contentItem: Text { text: parent.text; color: "#8A94A6"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5; implicitWidth: 80; implicitHeight: 30 }
                    onClicked: stackView.pop() 
                }
                Button {
                    text: "이벤트 영상"
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#2C75FF"; radius: 5; implicitWidth: 80; implicitHeight: 30 }
                }
            }
        }

        // ==========================================
        // 3. 메인 영상 플레이어 영역 (기존과 동일)
        // ==========================================
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 20
            color: "#141B2D"
            radius: 8
            border.color: "#1C2434"
            border.width: 1

            MediaPlayer {
                id: mediaPlayer
                audioOutput: AudioOutput {}
                videoOutput: videoOutput
                onErrorOccurred: console.log("미디어 재생 에러: ", mediaPlayer.errorString)
            }

            VideoOutput {
                id: videoOutput
                anchors.fill: parent
                anchors.margins: 2 
                fillMode: VideoOutput.PreserveAspectFit
            }

            Text {
                anchors.centerIn: parent
                text: "▶️ 비디오 재생 영역 (대기 중)"
                color: "#4A5568"
                font.pixelSize: 20
                visible: mediaPlayer.playbackState !== MediaPlayer.PlayingState
            }
            
            Button {
                anchors.bottom: parent.bottom; anchors.right: parent.right
                anchors.margins: 15
                text: "💾 영상 다운로드"
                contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                background: Rectangle { color: "#28A745"; radius: 5; implicitWidth: 120; implicitHeight: 35 }
                onClicked: {
                    if (currentPlayUrl === "") {
                        console.log("다운로드할 영상이 없습니다.")
                        return
                    }
                    sysController.downloadVideo(currentPlayUrl, currentFileName)
                }
            }
        }

        // ==========================================
        // 4. 하단 타임라인 컨트롤러 (기존과 동일)
        // ==========================================
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 120
            color: "#0B111E"
            Layout.leftMargin: 20; Layout.rightMargin: 20; Layout.bottomMargin: 20

            ColumnLayout {
                anchors.fill: parent
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "00:00:00"; color: "#8A94A6"; font.pixelSize: 12; font.family: "Courier" }
                    Slider {
                        id: timeSlider
                        Layout.fillWidth: true
                        value: mediaPlayer.position / Math.max(1, mediaPlayer.duration)
                        background: Rectangle {
                            x: timeSlider.leftPadding; y: timeSlider.topPadding + timeSlider.availableHeight / 2 - height / 2
                            implicitWidth: 200; implicitHeight: 4; width: timeSlider.availableWidth; height: implicitHeight; radius: 2; color: "#1C2434"
                            Rectangle { width: timeSlider.visualPosition * parent.width; height: parent.height; color: "#2C75FF"; radius: 2 }
                        }
                        handle: Rectangle {
                            x: timeSlider.leftPadding + timeSlider.visualPosition * (timeSlider.availableWidth - width)
                            y: timeSlider.topPadding + timeSlider.availableHeight / 2 - height / 2
                            implicitWidth: 14; implicitHeight: 14; radius: 7; color: timeSlider.pressed ? "#1A5BE6" : "#2C75FF"
                        }
                        onMoved: {
                            mediaPlayer.position = value * mediaPlayer.duration
                        }
                    }
                    Text { text: "01:00:00"; color: "#8A94A6"; font.pixelSize: 12; font.family: "Courier" }
                }

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
                        text: mediaPlayer.playbackState === MediaPlayer.PlayingState ? "⏸" : "▶"
                        contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 24; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { color: "#2C75FF"; radius: 25; implicitWidth: 50; implicitHeight: 50 }
                        onClicked: {
                            if (mediaPlayer.playbackState === MediaPlayer.PlayingState)
                                mediaPlayer.pause()
                            else
                                mediaPlayer.play()
                        }
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

    // ==========================================
    // 5. 우측 슬라이딩 이벤트 리스트 패널
    // ==========================================
    Rectangle {
        id: eventPanel
        width: 320
        height: parent.height - 60 
        y: 60
        x: isEventListOpen ? parent.width - width : parent.width 
        color: "#0F172A" 
        border.color: "#1C2434"
        border.width: 1

        Behavior on x { NumberAnimation { duration: 300; easing.type: Easing.OutQuart } }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 15
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Text { text: "🚨 낙상 이벤트 리스트"; color: "white"; font.pixelSize: 16; font.bold: true; Layout.fillWidth: true }
                Button {
                    text: "✕"
                    contentItem: Text { text: parent.text; color: "#8A94A6"; font.pixelSize: 16 }
                    background: Rectangle { color: "transparent" }
                    onClicked: isEventListOpen = false 
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: "#1C2434" } 

            ListView {
                id: eventListView
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 8

                model: ListModel { id: eventListModel }

                delegate: Rectangle {
                    width: eventListView.width
                    height: 60
                    color: "#1C2434"
                    radius: 6
                    
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: parent.color = "#2A3650"
                        onExited: parent.color = "#1C2434"
                        onClicked: {
                            currentFileName = "Event_" + model.eventId + "_CAM" + model.camId + ".mp4"
                            sysController.fetchVideoUrl(model.camId, model.eventId)
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        
                        Rectangle {
                            width: 36; height: 36; radius: 18; color: "#3D1A1A"
                            Text { anchors.centerIn: parent; text: "🚨"; font.pixelSize: 16 }
                        }
                        
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text { text: model.status + " (" + model.cam + ")"; color: "#FF4444"; font.pixelSize: 13; font.bold: true }
                            Text { text: model.time; color: "#8A94A6"; font.pixelSize: 12; font.family: "Courier" }
                        }
                        Text { text: "▶"; color: "#2C75FF"; font.pixelSize: 18 }
                    }
                }
            }
        }
    }
}