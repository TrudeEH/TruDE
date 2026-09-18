import Quickshell
import Quickshell.Io
import QtQuick
import "."

Rectangle {
    id: button

    property real cpuUsage: 0
    property real memoryUsage: 0
    property real memoryUsedGiB: 0
    property real memoryTotalGiB: 0
    property real previousIdle: -1
    property real previousTotal: -1

    readonly property string cpuText: Math.round(cpuUsage) + "%"
    readonly property string memoryText: Math.round(memoryUsage) + "%"

    width: 112
    height: 28
    radius: 8
    color: monitorMouse.containsMouse ? Theme.surfaceHover : Theme.transparent
    border.color: monitorMouse.containsMouse ? Theme.border : Theme.transparent
    border.width: 1

    Process {
        id: stats
        command: ["sh", "-c", "awk '/^cpu / && !seen { idle=$5+$6; total=$2+$3+$4+$5+$6+$7+$8+$9+$10; printf \"%.0f %.0f\\n\", idle, total; seen=1 } /^MemTotal:/ { memTotal=$2 } /^MemAvailable:/ { memAvailable=$2 } END { printf \"%.0f %.0f\\n\", memTotal, memAvailable }' /proc/stat /proc/meminfo"]
        running: false

        stdout: StdioCollector {
            onStreamFinished: {
                const rows = this.text.trim().split("\n")
                    .map(row => row.trim().split(/\s+/).map(Number));
                if (rows.length < 2 || rows[0].length < 2 || rows[1].length < 2)
                    return;

                const idle = rows[0][0];
                const total = rows[0][1];
                if (button.previousTotal >= 0 && total > button.previousTotal) {
                    const deltaTotal = total - button.previousTotal;
                    const deltaIdle = idle - button.previousIdle;
                    button.cpuUsage = Math.max(0, Math.min(100,
                        100 * (1 - deltaIdle / deltaTotal)));
                }
                button.previousIdle = idle;
                button.previousTotal = total;

                const memoryTotal = rows[1][0];
                const memoryAvailable = rows[1][1];
                button.memoryTotalGiB = memoryTotal / 1024 / 1024;
                button.memoryUsedGiB = (memoryTotal - memoryAvailable) / 1024 / 1024;
                button.memoryUsage = memoryTotal > 0
                    ? 100 * (memoryTotal - memoryAvailable) / memoryTotal : 0;
            }
        }
    }

    Timer {
        interval: 2000
        running: true
        repeat: true
        onTriggered: stats.running = true
    }

    Component.onCompleted: stats.running = true

    AppText {
        anchors.centerIn: parent
        text: "󰍛  " + button.cpuText + "    " + button.memoryText
        color: Theme.text
        font.pixelSize: 12
    }

    MouseArea {
        id: monitorMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: Quickshell.execDetached(["quickshell", "ipc", "call", "system-monitor", "toggle"])
    }
}
