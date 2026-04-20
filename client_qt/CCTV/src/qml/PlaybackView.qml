import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia // 👉 비디오 스트리밍 플레이어를 위해 추가

Item {
    id: playbackView

    property bool isEventListOpen: false 
    property string currentPlayUrl: ""
    property string currentFileName: "event_video.mp4"
    property date selectedDate: new Date(2026, 0, 1)

    // ==========================================
    // 👉 C++ SystemController의 시그널 수신부
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
            currentPlayUrl = streamUrl // 현재 재생 URL 기억
            mediaPlayer.source = streamUrl
            mediaPlayer.play() 
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ... (1. 상단 네비게이션 바는 이전과 동일) ...
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
        // 2. 컨트롤 바 
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
                Item { implicitWidth: 20 }
                
                ComboBox {
                    model: ["CAM 1", "CAM 2", "CAM 3", "CAM 4"]
                    implicitWidth: 120
                    contentItem: Text { text: parent.displayText; color: "white"; leftPadding: 10; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5 }
                }

                Button {
                    id: dateBtn
                    // 프로퍼티에 바인딩되어 날짜를 바꾸면 버튼 텍스트가 자동으로 바뀝니다.
                    text: "📅 " + Qt.formatDate(selectedDate, "yyyy. MM. dd.")
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5; implicitWidth: 130; implicitHeight: 30 }
                    
                    // 버튼 클릭 시 아래의 팝업 열기
                    onClicked: datePopup.open()

                    // ==========================================
                    // 날짜 선택 팝업 (Tumbler UI)
                    // ==========================================
                    Popup {
                        id: datePopup
                        y: dateBtn.height + 5 // 버튼 바로 아래에 스르륵 나타남
                        width: 240
                        height: 200
                        padding: 10
                        background: Rectangle { color: "#141B2D"; radius: 8; border.color: "#2C75FF"; border.width: 1 }

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 10
                            
                            // 스피닝 휠 (Tumbler) UI 영역
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                
                                // 년도 휠
                                Tumbler {
                                    id: yearTumbler
                                    model: 100 // 2000년 ~ 2099년
                                    currentIndex: selectedDate.getFullYear() - 2000
                                    delegate: Text {
                                        text: (2000 + index) + "년"
                                        // 중앙에 온 글자는 하얗고 크게, 벗어난 글자는 작고 흐리게
                                        color: Tumbler.displacement === 0 ? "white" : "#8A94A6"
                                        font.pixelSize: Tumbler.displacement === 0 ? 15 : 12
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }
                                // 월 휠
                                Tumbler {
                                    id: monthTumbler
                                    model: 12
                                    currentIndex: selectedDate.getMonth()
                                    delegate: Text {
                                        text: (index + 1) + "월"
                                        color: Tumbler.displacement === 0 ? "white" : "#8A94A6"
                                        font.pixelSize: Tumbler.displacement === 0 ? 15 : 12
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }
                                // 일 휠
                                Tumbler {
                                    id: dayTumbler
                                    model: 31
                                    currentIndex: selectedDate.getDate() - 1
                                    delegate: Text {
                                        text: (index + 1) + "일"
                                        color: Tumbler.displacement === 0 ? "white" : "#8A94A6"
                                        font.pixelSize: Tumbler.displacement === 0 ? 15 : 12
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }
                            }

                            // 확인/취소 버튼 영역
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
                                        
                                        // 1. 휠에서 선택한 값으로 날짜 업데이트 (버튼 텍스트는 자동 갱신됨)
                                        selectedDate = new Date(y, m, d)
                                        datePopup.close()
                                        
                                        // 2. 👉 메인 서버에 "선택한 날짜의 이벤트 목록" 재요청!
                                        // 해당 날짜의 00:00:00 부터 23:59:59 까지의 Unix Timestamp(ms)를 계산합니다.
                                        var startOfDay = new Date(y, m, d, 0, 0, 0).getTime()
                                        var endOfDay = new Date(y, m, d, 23, 59, 59).getTime()
                                        
                                        console.log("새로운 날짜 검색 ->", Qt.formatDate(selectedDate, "yyyy-MM-dd"))
                                        
                                        // 255(0xFF)는 '전체 카메라 조회'를 의미합니다.
                                        sysController.fetchEventList(255, startOfDay, endOfDay)
                                    }
                                }
                            }
                        }
                    }
                }

                // 👉 수정: 이벤트 목록 버튼 클릭 시 C++로 데이터 요청
                Button {
                    text: isEventListOpen ? "목록 닫기 ❌" : "이벤트 목록 📋"
                    contentItem: Text { text: parent.text; color: isEventListOpen ? "#FF4444" : "#00FF00"; font.pixelSize: 13; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5; implicitWidth: 110; implicitHeight: 30; border.color: isEventListOpen ? "#FF4444" : "#00FF00" }
                    onClicked: {
                        isEventListOpen = !isEventListOpen
                        if (isEventListOpen) {
                            // C++로 리스트 요청 (camId: 255(0xFF 전체), 임의의 timestamp 0)
                            sysController.fetchEventList(255, 0, 0)
                        }
                    }
                }

                ComboBox {
                    model: ["1x", "2x", "4x", "8x"]
                    implicitWidth: 70
                    contentItem: Text { text: parent.displayText; color: "white"; leftPadding: 10; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { color: "#1C2434"; radius: 5 }
                }

                Item { Layout.fillWidth: true }

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
        // 3. 메인 영상 플레이어 영역 (MediaPlayer 도입)
        // ==========================================
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 20
            color: "#141B2D"
            radius: 8
            border.color: "#1C2434"
            border.width: 1

            // 👉 추가: 실제 영상을 재생할 플레이어
            MediaPlayer {
                id: mediaPlayer
                audioOutput: AudioOutput {}
                videoOutput: videoOutput
                
                // 재생 에러 발생 시 로그 출력
                onErrorOccurred: console.log("미디어 재생 에러: ", mediaPlayer.errorString)
            }

            VideoOutput {
                id: videoOutput
                anchors.fill: parent
                anchors.margins: 2 // 테두리 보호
                fillMode: VideoOutput.PreserveAspectFit
            }

            // 영상이 재생 중이지 않을 때만 보여줄 플레이스홀더 텍스트
            Text {
                anchors.centerIn: parent
                text: "▶️ 비디오 재생 영역 (대기 중)"
                color: "#4A5568"
                font.pixelSize: 20
                visible: mediaPlayer.playbackState !== MediaPlayer.PlayingState
            }
            
            // 👉 추가: 영상 다운로드(저장) 버튼
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
                    // C++의 다운로드 함수 호출
                    sysController.downloadVideo(currentPlayUrl, currentFileName)
                }
            }
        }

        // ==========================================
        // 4. 하단 타임라인 컨트롤러 (재생/정지 버튼 연동)
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
                        // 배경, 핸들 디자인은 이전과 동일 (생략 없이 유지)
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
                            // 슬라이더 이동 시 미디어 위치 변경
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

                // 👉 수정: 더미 데이터를 지우고 빈 ListModel 생성
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
                            // 👉 파일명 자동 생성 (예: Event_104_CAM1.mp4)
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