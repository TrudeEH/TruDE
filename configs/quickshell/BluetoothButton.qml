import Quickshell
import Quickshell.Io
import QtQuick
import "."

Rectangle {
    id: button

    property bool powered: false
    property string connectedDevice: ""

    width: Math.max(104, bluetoothLabel.implicitWidth + 20)
    height: 28
    radius: 8
    color: bluetoothMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised
    border.color: bluetoothMouse.containsMouse ? Theme.border : Theme.transparent
    border.width: 1

    Process {
        id: status
        command: ["sh", "-c", "powered=$(bluetoothctl show 2>/dev/null | awk -F': ' '/Powered:/ { print $2; exit }'); connected=$(bluetoothctl devices Connected 2>/dev/null | sed -E 's/^Device [^ ]+ //' | head -n1); printf '%s\\t%s\\n' \"$powered\" \"$connected\""]
        running: false
        stdout: StdioCollector {
            onStreamFinished: {
                const fields = this.text.trim().split("\t");
                button.powered = fields[0] === "yes";
                button.connectedDevice = fields.length > 1 ? fields.slice(1).join("\t") : "";
            }
        }
    }

    Timer {
        interval: 3000
        running: true
        repeat: true
        onTriggered: status.running = true
    }

    Component.onCompleted: status.running = true

    AppText {
        id: bluetoothLabel
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        text: "󰂯  " + (button.powered
            ? (button.connectedDevice.length > 0 ? button.connectedDevice : "Bluetooth")
            : "Bluetooth off")
        color: button.powered ? Theme.text : Theme.textDim
        font.pixelSize: 12
        elide: Text.ElideRight
    }

    MouseArea {
        id: bluetoothMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: Quickshell.execDetached(["quickshell", "ipc", "call", "bluetooth", "toggle"])
    }
}
