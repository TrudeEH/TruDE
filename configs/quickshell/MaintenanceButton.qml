import Quickshell
import Quickshell.Io
import QtQuick
import QtQuick.Layouts
import "."

Rectangle {
    id: button

    property int aptUpdates: 0
    property int flatpakUpdates: 0

    width: 136
    height: 28
    radius: 8
    color: maintenanceMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised
    border.color: maintenanceMouse.containsMouse ? Theme.border : Theme.transparent
    border.width: 1

    Process {
        id: counts
        command: ["sh", "-c", "apt list --upgradable 2>/dev/null | tail -n +2 | sed '/^$/d' | wc -l; if command -v flatpak >/dev/null 2>&1; then flatpak remote-ls --updates --app 2>/dev/null | sed '/^$/d' | wc -l; else printf '0\\n'; fi"]
        running: false

        stdout: StdioCollector {
            onStreamFinished: {
                const values = this.text.trim().split("\n").map(Number);
                if (values.length >= 2) {
                    button.aptUpdates = values[0] || 0;
                    button.flatpakUpdates = values[1] || 0;
                }
            }
        }
    }

    Timer {
        interval: 30000
        running: true
        repeat: true
        onTriggered: counts.running = true
    }

    Component.onCompleted: counts.running = true

    RowLayout {
        anchors.fill: parent
        anchors.margins: 2
        spacing: 3

        MetricPill {
            Layout.fillWidth: true
            icon: "󰏗"
            label: "APT"
            value: button.aptUpdates
            highlighted: button.aptUpdates > 0
        }

        MetricPill {
            Layout.fillWidth: true
            icon: "󰏖"
            label: "FLAT"
            value: button.flatpakUpdates
            highlighted: button.flatpakUpdates > 0
        }
    }

    MouseArea {
        id: maintenanceMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: Quickshell.execDetached(["quickshell", "ipc", "call", "maintenance", "toggle"])
    }
}
