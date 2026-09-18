import Quickshell
import Quickshell.Io
import QtQuick
import "."

Rectangle {
    id: button

    property string profile: "balanced"
    readonly property string profileLabel: profile === "power-saver" ? "Power saver"
        : profile.charAt(0).toUpperCase() + profile.slice(1)

    width: Math.max(104, profileLabelText.implicitWidth + 20)
    height: 28
    radius: 8
    color: profileMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised
    border.color: profileMouse.containsMouse ? Theme.border : Theme.transparent
    border.width: 1

    Process {
        id: currentProfile
        command: ["powerprofilesctl", "get"]
        running: false
        stdout: StdioCollector {
            onStreamFinished: {
                const value = this.text.trim();
                if (value.length > 0) button.profile = value;
            }
        }
    }

    Timer {
        interval: 5000
        running: true
        repeat: true
        onTriggered: currentProfile.running = true
    }

    Component.onCompleted: currentProfile.running = true

    AppText {
        id: profileText
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        text: "󰾅  " + button.profileLabel
        color: Theme.text
        font.pixelSize: 12
        elide: Text.ElideRight
    }

    Text {
        id: profileLabelText
        visible: false
        text: "󰾅  " + button.profileLabel
        font.family: Theme.fontFamily
        font.pixelSize: 12
    }

    MouseArea {
        id: profileMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: Quickshell.execDetached(["quickshell", "ipc", "call", "power-profile", "toggle"])
    }
}
