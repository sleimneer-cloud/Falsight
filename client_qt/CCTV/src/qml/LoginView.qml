import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: loginView

    // C++ 컨트롤러(SystemController)에서 보내는 시그널 수신
    Connections {
        target: sysController
        function onLoginResult(success, message) {
            if (success) {
                console.log("로그인 성공! 다음 화면으로 넘어갑니다.")
                // 수정된 부분: 로그인 화면을 모니터링 화면으로 완전히 교체합니다.
                stackView.replace("MonitorView.qml") 
            } else {
                errorDialog.text = message
                errorDialog.open()
            }
        }
    }

    // 중앙 로그인 카드 레이아웃
    Rectangle {
        width: 400
        height: 500
        anchors.centerIn: parent
        color: "#1C2434"
        radius: 10

        ColumnLayout {
            anchors.centerIn: parent
            width: 320
            spacing: 20

            // 타이틀 영역
            ColumnLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 5
                
                Text {
                    text: "CCTV 모니터링 시스템"
                    color: "white"
                    font.pixelSize: 22
                    font.bold: true
                    Layout.alignment: Qt.AlignHCenter
                }
                Text {
                    text: "로그인하여 모니터링을 시작하세요"
                    color: "#8A94A6"
                    font.pixelSize: 13
                    Layout.alignment: Qt.AlignHCenter
                }
            }

            Item { Layout.preferredHeight: 10 } // 여백

            // 아이디 입력
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5
                Text { text: "아이디"; color: "#8A94A6"; font.pixelSize: 12 }
                TextField {
                    id: idInput
                    Layout.fillWidth: true
                    placeholderText: "아이디를 입력하세요"
                    color: "white"
                    background: Rectangle { color: "#263043"; radius: 6 }
                    leftPadding: 15; topPadding: 10; bottomPadding: 10
                }
            }

            // 비밀번호 입력
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5
                Text { text: "비밀번호"; color: "#8A94A6"; font.pixelSize: 12 }
                TextField {
                    id: pwInput
                    Layout.fillWidth: true
                    placeholderText: "비밀번호를 입력하세요"
                    color: "white"
                    echoMode: TextInput.Password
                    background: Rectangle { color: "#263043"; radius: 6 }
                    leftPadding: 15; topPadding: 10; bottomPadding: 10
                    
                    // 엔터키 누르면 로그인 실행
                    Keys.onReturnPressed: loginBtn.clicked() 
                    Keys.onEnterPressed: loginBtn.clicked()
                }
            }

            Item { Layout.preferredHeight: 10 } // 여백

            // 로그인 버튼
            Button {
                id: loginBtn
                Layout.fillWidth: true
                contentItem: Text {
                    text: "로그인"
                    color: "white"
                    font.bold: true
                    font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: loginBtn.down ? "#1A5BE6" : "#2C75FF"
                    radius: 6
                    implicitHeight: 45
                }
                onClicked: {
                    // QML에서 C++ 백엔드 함수 직접 호출!
                    sysController.requestLogin(idInput.text, pwInput.text)
                }
            }

            Text {
                text: "초기 아이디/비밀번호 : admin / admin"
                color: "#4A5568"
                font.pixelSize: 11
                Layout.alignment: Qt.AlignHCenter
            }
        }
    }


// 에러 메시지 팝업 (Popup으로 변경하여 디자인 완전 커스텀)
    Popup {
        id: errorDialog
        anchors.centerIn: parent
        width: 300
        height: 160
        modal: true // 배경을 어둡게 하고 클릭을 막음
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        property alias text: errorMsg.text

        background: Rectangle { 
            color: "#1C2434"
            radius: 10
            border.color: "#263043"
            border.width: 1
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 10

            // 팝업 타이틀
            Text {
                text: "로그인 실패"
                color: "white"
                font.pixelSize: 16
                font.bold: true
                Layout.alignment: Qt.AlignHCenter
            }

            // 에러 메시지 내용
            Text {
                id: errorMsg
                color: "#8A94A6"
                font.pixelSize: 13
                Layout.alignment: Qt.AlignHCenter
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap // 글자가 길면 줄바꿈
            }

            Item { Layout.fillHeight: true } // 빈 공간 밀어내기

            // 확인 버튼
            Button {
                Layout.fillWidth: true
                contentItem: Text { 
                    text: "확인"
                    color: "white"
                    font.pixelSize: 14
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { 
                    color: parent.down ? "#1A5BE6" : "#2C75FF"
                    radius: 6
                    implicitHeight: 40
                }
                onClicked: errorDialog.close()
            }
        }
    }
}