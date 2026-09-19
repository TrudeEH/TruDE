import Quickshell
import Quickshell.Io
import QtQuick
import QtQuick.Layouts
import "."

Rectangle {
    id: button

    property real cpuUsage: 0
    property real memoryUsage: 0
    property real maxTemperature: 0
    property real previousIdle: -1
    property real previousTotal: -1

    width: 196
    height: 28
    radius: 8
    color: monitorMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised
    border.color: monitorMouse.containsMouse ? Theme.border : Theme.transparent
    border.width: 1

    Process {
        id: stats
        command: ["sh", "-c", "awk '/^cpu / && !seen { print $5+$6, $2+$3+$4+$5+$6+$7+$8+$9+$10; seen=1 } /^MemTotal:/ { print $2 } /^MemAvailable:/ { print $2 }' /proc/stat /proc/meminfo; max=0; for input in /sys/class/hwmon/hwmon*/temp*_input /sys/class/thermal/thermal_zone*/temp; do [ -r \"$input\" ] || continue; value=$(cat \"$input\" 2>/dev/null) || continue; [ \"$value\" -gt \"$max\" ] && max=$value; done; printf '%s\\n' \"$max\""]
        running: false

        stdout: StdioCollector {
            onStreamFinished: {
                const rows = this.text.trim().split("\n")
                    .map(row => row.trim().split(/\s+/).map(Number));
                if (rows.length < 4 || rows[0].length < 2 || rows[1].length < 1
                        || rows[2].length < 1 || rows[3].length < 1)
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
                const memoryAvailable = rows[2][0];
                button.memoryUsage = memoryTotal > 0
                    ? 100 * (memoryTotal - memoryAvailable) / memoryTotal : 0;
                button.maxTemperature = rows[3][0] > 0 ? rows[3][0] / 1000 : 0;
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

    RowLayout {
        anchors.fill: parent
        anchors.margins: 2
        spacing: 3

        MetricPill {
            Layout.fillWidth: true
            icon: "󰍛"
            label: ""
            value: Math.round(button.cpuUsage) + "%"
        }

        MetricPill {
            Layout.fillWidth: true
            icon: ""
            label: ""
            value: Math.round(button.memoryUsage) + "%"
        }

        MetricPill {
            Layout.fillWidth: true
            icon: ""
            label: ""
            value: button.maxTemperature > 0
                ? Math.round(button.maxTemperature) + "°C" : "—"
        }
    }

    MouseArea {
        id: monitorMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: Quickshell.execDetached([
            "foot",
            "--app-id=btop",
            "--title=System Monitor",
            "--window-size-chars=120x36",
            "dotfiles-system-monitor-tui"
        ])
    }
}
