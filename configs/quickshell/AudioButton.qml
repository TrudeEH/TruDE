import Quickshell
import Quickshell.Services.Pipewire
import QtQuick
import "."

Rectangle {
    id: button
    readonly property var sink: Pipewire.defaultAudioSink
    readonly property int percentage: sink && sink.audio ? Math.round(sink.audio.volume * 100) : 0
    readonly property string icon: !sink || !sink.audio || sink.audio.muted
        ? "󰖁" : percentage === 0 ? "󰕿" : percentage < 50 ? "󰖀" : "󰕾"

    width: 76
    height: 28
    radius: 8
    color: buttonMouse.containsMouse ? Theme.surfaceHover : Theme.transparent
    border.color: buttonMouse.containsMouse ? Theme.border : Theme.transparent
    border.width: 1

    AppText {
        anchors.centerIn: parent
        text: button.icon + "  " + button.percentage + "%"
        color: Theme.text
        font.pixelSize: 12
    }

    MouseArea {
        id: buttonMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: Quickshell.execDetached(["quickshell", "ipc", "call", "audio", "toggle"])
    }
}
