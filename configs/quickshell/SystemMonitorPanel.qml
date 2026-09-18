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
    property string uptimeText: "—"
    property string loadText: "—"
    property string cpuText: "—"
    property string memoryText: "—"
    property string swapText: "—"
    property string diskText: "—"
    property var gpuCards: []
    property string batteryText: "—"
    readonly property var focusedScreen: {
        const focused = Hyprland.focusedMonitor;
        if (!focused) return null;
        for (const screen of Quickshell.screens) {
            if (screen.name === focused.name) return screen;
        }
        return null;
    }

    readonly property string detailsCommand:
        "printf 'uptime\\t%s\\n' \"$(uptime -p)\"; "
        + "printf 'load\\t%s\\n' \"$(awk '{print $1 \"  \" $2 \"  \" $3}' /proc/loadavg)\"; "
        + "printf 'cpu\\t%s\\n' \"$(lscpu 2>/dev/null | awk -F: '/Model name/ {gsub(/^[ \\t]+/, \"\", $2); print $2; exit}')\"; "
        + "printf 'memory\\t%s\\n' \"$(free -h | awk '/^Mem:/ {print $3 \" / \" $2}')\"; "
        + "printf 'swap\\t%s\\n' \"$(free -h | awk '/^Swap:/ {print $3 \" / \" $2}')\"; "
        + "printf 'disk\\t%s\\n' \"$(df -h / | awk 'NR==2 {print $3 \" / \" $2 \" (\" $5 \" used)\"}')\"; "
        + "gpuFound=0; for card in /sys/class/drm/card[0-9]; do busy=\$(cat \"\$card/device/gpu_busy_percent\" 2>/dev/null) || continue; used=\$(cat \"\$card/device/mem_info_vram_used\" 2>/dev/null); total=\$(cat \"\$card/device/mem_info_vram_total\" 2>/dev/null); slot=\$(awk -F= '/^PCI_SLOT_NAME=/ {print \$2; exit}' \"\$card/device/uevent\" 2>/dev/null); name=\$(if [ -n \"\$slot\" ] && command -v lspci >/dev/null 2>&1; then lspci -s \"\$slot\" 2>/dev/null | sed -E 's/^[^ ]+ [^:]+: //'; else basename \"\$card\"; fi); [ -n \"\$name\" ] || name=\$(basename \"\$card\"); shortName=\$(printf '%s\\n' \"\$name\" | sed -E 's/.*\\[Radeon (RX [0-9]+ [^/]+)\\/.*/\\1/; s/.*\\b(Raphael)\\b.*/\\1/'); [ -n \"\$shortName\" ] && name=\"\$shortName\"; usedGiB=\$(awk -v bytes=\"\$used\" 'BEGIN {printf \"%.1f\", bytes / 1073741824}'); totalGiB=\$(awk -v bytes=\"\$total\" 'BEGIN {printf \"%.1f\", bytes / 1073741824}'); printf 'gpu\\t%s\\t%s\\t%s\\t%s\\n' \"\$name\" \"\$busy\" \"\$usedGiB\" \"\$totalGiB\"; gpuFound=1; done; [ \"\$gpuFound\" -eq 1 ] || printf 'gpu\\tUnavailable\\t0\\t0\\t0\\n'; "
        + "printf 'battery\\t%s\\n' \"$(if command -v upower >/dev/null 2>&1; then battery=\$(upower -e 2>/dev/null | grep -m1 battery); [ -n \"\$battery\" ] && upower -i \"\$battery\" | awk -F: '/percentage/ {gsub(/^[ \\t]+/, \"\", $2); print $2}'; else printf 'Unavailable'; fi)\""

    function close() {
        popupOpen = false;
        popupScreen = null;
    }

    function closeOtherPopups() {
        PopupManager.closeExcept("system-monitor");
    }

    function applyDetails(output) {
        gpuCards = [];
        const lines = output.trim().split("\n");
        for (const line of lines) {
            const fields = line.split("\t");
            if (fields.length < 2) continue;
            const key = fields[0];
            const value = fields.slice(1).join("\t").trim() || "Unavailable";
            if (key === "uptime") uptimeText = value;
            else if (key === "load") loadText = value;
            else if (key === "cpu") cpuText = value;
            else if (key === "memory") memoryText = value;
            else if (key === "swap") swapText = value;
            else if (key === "disk") diskText = value;
            else if (key === "gpu" && fields.length >= 5) {
                gpuCards = gpuCards.concat({ name: fields[1] || "GPU", usage: fields[2] || "0", used: fields[3] || "0.0", total: fields[4] || "0.0" });
            }
            else if (key === "battery") batteryText = value;
        }
    }

    function refresh() {
        uptimeText = "Collecting…";
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
        function toggle(): void { monitor.toggle(); }
        function close(): void { monitor.close(); }
    }

    Process {
        id: details
        command: ["sh", "-c", monitor.detailsCommand]
        running: false
        stdout: StdioCollector {
            onStreamFinished: monitor.applyDetails(this.text)
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
            width: 560
            height: Math.min(parent.height - 24, 700)
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
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Rectangle {
                        implicitWidth: 32
                        implicitHeight: 32
                        radius: 10
                        color: Theme.accent
                        AppText {
                            anchors.centerIn: parent
                            text: "󰍛"
                            color: Theme.accentText
                            font.pixelSize: 17
                        }
                    }

                    ColumnLayout {
                        spacing: 2
                        AppText {
                            text: "System monitor"
                            color: Theme.text
                            font.pixelSize: 20
                            font.bold: true
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Rectangle {
                        Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
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

                Flickable {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: width
                    contentHeight: monitorGrid.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    GridLayout {
                        id: monitorGrid
                        width: parent.width
                        columns: 2
                        columnSpacing: 8
                        rowSpacing: 8

                        InfoCard { Layout.fillWidth: true; icon: "󰍛"; title: "CPU"; value: monitor.cpuText; detail: "Processor" }
                        InfoCard { Layout.fillWidth: true; icon: ""; title: "Memory"; value: monitor.memoryText; detail: "Used / total" }
                        InfoCard { Layout.fillWidth: true; icon: "󰾆"; title: "Swap"; value: monitor.swapText; detail: "Used / total" }
                        InfoCard { Layout.fillWidth: true; icon: "󰋊"; title: "Disk"; value: monitor.diskText; detail: "Root filesystem" }

                        Repeater {
                            model: monitor.gpuCards
                            delegate: InfoCard {
                                required property var modelData
                                Layout.fillWidth: true
                                icon: "󰢮"
                                title: modelData.name
                                value: modelData.usage + "% usage"
                                detail: modelData.used + " / " + modelData.total + " GiB VRAM"
                                highlighted: Number(modelData.usage) > 80
                            }
                        }
                        InfoCard { Layout.fillWidth: true; icon: "󰂄"; title: "Battery"; value: monitor.batteryText; detail: "Power status" }
                        InfoCard { Layout.fillWidth: true; icon: "󰅐"; title: "Uptime"; value: monitor.uptimeText; detail: "Since last boot" }
                        InfoCard { Layout.fillWidth: true; icon: "󰓅"; title: "Load"; value: monitor.loadText; detail: "1 / 5 / 15 minutes" }
                    }
                }
            }
        }
    }
}
