import Quickshell
import QtQuick
import "."

Rectangle {
    id: button

    required property string icon
    required property var command
    property string label: ""
    signal scrolled(int steps)

    implicitWidth: label.length > 0 ? content.implicitWidth + 22 : 28
    width: implicitWidth
    height: 28
    radius: 0
    color: buttonMouse.containsMouse ? Theme.accentStrong : Theme.surfaceRaised
    border.color: buttonMouse.containsMouse ? Theme.accent : Theme.transparent
    border.width: 1

    AppText {
        id: content
        anchors.centerIn: parent
        text: button.label.length > 0
            ? button.icon + "  " + button.label
            : button.icon
        color: buttonMouse.containsMouse ? Theme.accentText : Theme.text
        font.pixelSize: 13
        font.weight: Font.Medium
    }

    MouseArea {
        id: buttonMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: Quickshell.execDetached(button.command)
        onWheel: wheel => {
            button.scrolled(wheel.angleDelta.y > 0 ? 1 : -1);
            wheel.accepted = true;
        }
    }
}
