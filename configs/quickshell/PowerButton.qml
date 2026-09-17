import Quickshell
import QtQuick
import "."

Rectangle {
    id: button
    width: 28
    height: 28
    radius: 8
    color: buttonMouse.containsMouse ? Theme.surfaceHover : Theme.transparent
    border.color: buttonMouse.containsMouse ? Theme.border : Theme.transparent
    border.width: 1

    Text {
        anchors.centerIn: parent
        text: "⏻"
        color: Theme.text
        font.pixelSize: 18
    }

    MouseArea {
        id: buttonMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: Quickshell.execDetached(["quickshell", "ipc", "call", "power", "toggle"])
    }
}
