import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import Quickshell.Wayland
import QtQuick
import QtQuick.Layouts
import "."

Item {
    id: bluetooth

    property bool popupOpen: false
    property int popupMonitorId: -1
    property bool available: false
    property bool powered: false
    property string errorText: ""
    property var devices: []
    readonly property var focusedScreen: {
        const focused = Hyprland.focusedMonitor;
        if (!focused) return null;
        for (const screen of Quickshell.screens) {
            if (screen.name === focused.name) return screen;
        }
        return null;
    }

    function close() {
        popupOpen = false;
        popupMonitorId = -1;
    }

    function closeOtherPopups() {
        PopupManager.closeExcept("bluetooth");
    }

    function refresh() {
        status.running = true;
        deviceList.running = true;
    }

    function run(command) {
        errorText = "";
        Quickshell.execDetached(command);
        refreshTimer.restart();
    }

    Process {
        id: status
        command: ["sh", "-c", "if command -v bluetoothctl >/dev/null 2>&1; then bluetoothctl show 2>/dev/null | awk -F': ' '/Powered:/ { print $2; exit }'; else printf 'missing\n'; fi"]
        running: false
        stdout: StdioCollector {
            onStreamFinished: {
                bluetooth.available = this.text.trim() !== "missing";
                bluetooth.powered = this.text.trim() === "yes";
            }
        }
    }

    Process {
        id: deviceList
        command: ["sh", "-c", "bluetoothctl devices 2>/dev/null | while read -r kind mac name; do connected=$(bluetoothctl info \"$mac\" 2>/dev/null | awk -F': ' '/Connected:/ { print $2; exit }'); printf '%s\\t%s\\t%s\\n' \"$connected\" \"$mac\" \"$name\"; done"]
        running: false
        stdout: StdioCollector {
            onStreamFinished: {
                const rows = this.text.trim().length > 0 ? this.text.trim().split("\n") : [];
                bluetooth.devices = rows.map(row => {
                    const fields = row.split("\t");
                    return {
                        connected: fields[0] === "yes",
                        address: fields[1] || "",
                        name: fields.slice(2).join("\t") || fields[1] || "Unknown device"
                    };
                }).filter(device => device.address.length > 0);
            }
        }
    }

    Timer {
        id: refreshTimer
        interval: 800
        repeat: false
        onTriggered: bluetooth.refresh()
    }

    Timer {
        interval: 5000
        running: true
        repeat: true
        onTriggered: bluetooth.refresh()
    }

    Component.onCompleted: refresh()

    IpcHandler {
        target: "bluetooth"

        function close(): void {
            bluetooth.close();
        }

        function toggle(): void {
            if (bluetooth.popupOpen) {
                bluetooth.close();
                return;
            }
            bluetooth.closeOtherPopups();
            if (!Hyprland.focusedMonitor) return;
            bluetooth.popupMonitorId = Hyprland.focusedMonitor.id;
            bluetooth.popupOpen = true;
            bluetooth.refresh();
        }
    }

    Variants {
        model: Quickshell.screens
        delegate: Component {
            PanelWindow {
                required property var modelData
                screen: modelData
                readonly property var monitor: Hyprland.monitorFor(screen)
                visible: bluetooth.popupOpen && monitor && monitor.id === bluetooth.popupMonitorId
                color: Theme.transparent
                anchors { top: true; bottom: true; left: true; right: true }

                WlrLayershell.namespace: "hyprland-bluetooth"
                WlrLayershell.layer: WlrLayer.Overlay
                WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

                Rectangle {
                    anchors.fill: parent
                    color: Theme.overlay
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: bluetooth.close()
                }

                Rectangle {
                    id: card
                    width: Math.min(360, parent.width - 24)
                    height: Math.min(430, 158 + bluetooth.devices.length * 58)
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.topMargin: 8
                    anchors.rightMargin: 14
                    radius: 12
                    color: Theme.surface
                    border.color: Theme.border
                    border.width: 1
                    focus: true
                    Keys.onEscapePressed: bluetooth.close()

                    MouseArea {
                        anchors.fill: parent
                        onClicked: mouse.accepted = true
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            AppText {
                                Layout.fillWidth: true
                                text: "Bluetooth"
                                color: Theme.text
                                font.pixelSize: 16
                                font.bold: true
                            }
                            Rectangle {
                                implicitWidth: 76
                                implicitHeight: 28
                                radius: 8
                                color: bluetooth.available && bluetooth.powered ? Theme.accent : Theme.surfaceRaised
                                border.color: bluetooth.powered ? Theme.accent : Theme.border
                                border.width: 1
                                AppText {
                                    anchors.centerIn: parent
                                    text: !bluetooth.available ? "N/A" : bluetooth.powered ? "On" : "Off"
                                    color: bluetooth.available && bluetooth.powered ? Theme.accentText : Theme.textDim
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    enabled: bluetooth.available
                                    onClicked: bluetooth.run(["bluetoothctl", "power", bluetooth.powered ? "off" : "on"])
                                }
                            }
                        }

                        AppText {
                            Layout.fillWidth: true
                            text: !bluetooth.available ? "Install bluez to manage Bluetooth devices"
                                : bluetooth.powered ? "Paired and nearby devices" : "Turn Bluetooth on to manage devices"
                            color: Theme.textDim
                            font.pixelSize: 11
                        }

                        Repeater {
                            model: bluetooth.devices
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: 48
                                radius: 8
                                color: deviceMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised
                                border.color: modelData.connected ? Theme.accent : Theme.transparent
                                border.width: 1

                                ColumnLayout {
                                    anchors.left: parent.left
                                    anchors.right: action.left
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 8
                                    spacing: 1
                                    AppText {
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        color: Theme.text
                                        font.pixelSize: 12
                                        elide: Text.ElideRight
                                    }
                                    AppText {
                                        Layout.fillWidth: true
                                        text: modelData.connected ? "Connected" : modelData.address
                                        color: modelData.connected ? Theme.accent : Theme.textDim
                                        font.pixelSize: 10
                                    }
                                }

                                Rectangle {
                                    id: action
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 70
                                    height: 28
                                    radius: 7
                                    color: modelData.connected ? Theme.accent : Theme.surface
                                    AppText {
                                        anchors.centerIn: parent
                                        text: modelData.connected ? "Disconnect" : "Connect"
                                        color: modelData.connected ? Theme.accentText : Theme.text
                                        font.pixelSize: 10
                                    }
                                    MouseArea {
                                        id: deviceMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: bluetooth.run(["bluetoothctl", modelData.connected ? "disconnect" : "connect", modelData.address])
                                    }
                                }
                            }
                        }

                        Item { Layout.fillHeight: true }

                        AppText {
                            visible: bluetooth.available && bluetooth.devices.length === 0
                            Layout.fillWidth: true
                            text: "No Bluetooth devices found"
                            color: Theme.textDim
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
            }
        }
    }
}
