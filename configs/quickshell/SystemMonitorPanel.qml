import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import Quickshell.Wayland
import QtQuick
import QtQuick.Layouts
import "."

Item {
    id: monitor

    property bool popupOpen: false
    property var popupScreen: null
    property string detailsText: "Collecting system information..."
    readonly property var focusedScreen: {
        const focused = Hyprland.focusedMonitor;
        if (!focused) return null;
        for (const screen of Quickshell.screens) {
            if (screen.name === focused.name) return screen;
        }
        return null;
    }

    readonly property string detailsCommand:
        "printf '%s\\n' 'UPTIME'; uptime -p; "
        + "printf '\\n%s\\n' 'LOAD'; cat /proc/loadavg; "
        + "printf '\\n%s\\n' 'CPU'; lscpu 2>/dev/null | awk -F: '/Model name/ {gsub(/^[ \\t]+/, \"\", $2); print $2; exit}'; "
        + "printf '\\n%s\\n' 'MEMORY'; free -h; "
        + "printf '\\n%s\\n' 'DISK'; df -hT --output=target,fstype,size,used,avail,pcent 2>/dev/null | awk 'NR==1 || $1==\"/\" || $1==\"/home\"'; "
        + "printf '\\n%s\\n' 'GPU'; if command -v nvidia-smi >/dev/null 2>&1; then nvidia-smi --query-gpu=name,utilization.gpu,memory.used,memory.total,temperature.gpu --format=csv,noheader; elif command -v lspci >/dev/null 2>&1; then lspci | grep -Ei 'vga|3d|display' || printf '%s\\n' 'No GPU information available'; else printf '%s\\n' 'Install pciutils for GPU information'; fi; "
        + "printf '\\n%s\\n' 'BATTERY'; if command -v upower >/dev/null 2>&1; then upower -e 2>/dev/null | grep battery | while read -r battery; do upower -i \"$battery\" | awk -F: '/state|percentage|time to empty/ {gsub(/^[ \\t]+/, \"\", $2); print $1 \" \" $2}'; done; else printf '%s\\n' 'No UPower battery information available'; fi"

    function close() {
        popupOpen = false;
        popupScreen = null;
    }

    function closeOtherPopups() {
        PopupManager.closeExcept("system-monitor");
    }

    function refresh() {
        detailsText = "Collecting system information...";
        details.running = true;
    }

    function toggle() {
        if (popupOpen) {
            close();
            return;
        }
        closeOtherPopups();
        popupScreen = focusedScreen;
        popupOpen = popupScreen !== null;
        if (popupOpen) refresh();
    }

    IpcHandler {
        target: "system-monitor"

        function toggle(): void {
            monitor.toggle();
        }

        function close(): void {
            monitor.close();
        }
    }

    Process {
        id: details
        command: ["sh", "-c", monitor.detailsCommand]
        running: false
        stdout: StdioCollector {
            onStreamFinished: monitor.detailsText = this.text.trim()
        }
    }

    PanelWindow {
        screen: monitor.popupScreen
        visible: monitor.popupOpen && monitor.popupScreen !== null
        color: Theme.transparent
        anchors { top: true; bottom: true; left: true; right: true }

        WlrLayershell.namespace: "hyprland-system-monitor"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

        MouseArea {
            anchors.fill: parent
            onClicked: monitor.close()
        }

        Rectangle {
            id: card
            width: 500
            height: Math.min(parent.height - 24, 650)
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: 8
            anchors.rightMargin: 14
            radius: 12
            color: Theme.surface
            border.color: Theme.border
            border.width: 1
            focus: true

            Keys.onEscapePressed: monitor.close()

            MouseArea {
                anchors.fill: parent
                onClicked: mouse.accepted = true
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        AppText {
                            text: "System monitor"
                            color: Theme.text
                            font.pixelSize: 20
                            font.bold: true
                        }
                        AppText {
                            text: "Live hardware and resource details"
                            color: Theme.textDim
                            font.pixelSize: 12
                        }
                    }

                    Rectangle {
                        implicitWidth: 34
                        implicitHeight: 34
                        radius: 17
                        color: refreshMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised

                        AppText {
                            anchors.centerIn: parent
                            text: "󰑐"
                            color: Theme.text
                            font.pixelSize: 16
                        }
                        MouseArea {
                            id: refreshMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: monitor.refresh()
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 8
                    color: Theme.window
                    border.color: Theme.border
                    border.width: 1

                    Flickable {
                        anchors.fill: parent
                        anchors.margins: 12
                        contentWidth: width
                        contentHeight: detailsLabel.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds

                        AppText {
                            id: detailsLabel
                            width: parent.width
                            text: monitor.detailsText
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                        }
                    }
                }
            }
        }
    }
}
