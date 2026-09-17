import Quickshell
import QtQuick
import "."

Rectangle {
    width: 48
    height: 24
    radius: 4
    color: launcherMouse.containsMouse ? Theme.surfaceHover : Theme.surface

    Text {
        anchors.centerIn: parent
        text: "Apps"
        color: Theme.text
        font.pixelSize: 12
    }

    MouseArea {
        id: launcherMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: Quickshell.execDetached(["fuzzel"])
    }
}
