import Quickshell
import QtQuick
import "."

Rectangle {
    width: 72
    height: 28
    radius: 8
    color: launcherMouse.containsMouse ? Theme.surfaceHover : Theme.transparent
    border.color: launcherMouse.containsMouse ? Theme.border : Theme.transparent
    border.width: 1

    AppText {
        anchors.centerIn: parent
        text: "󰀻  Apps"
        color: Theme.text
        font.pixelSize: 13
        font.weight: Font.Medium
    }

    MouseArea {
        id: launcherMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: Quickshell.execDetached(["quickshell", "ipc", "call", "launcher", "toggle"])
    }
}
