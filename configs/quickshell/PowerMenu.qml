import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import Quickshell.Wayland
import QtQuick
import QtQuick.Layouts
import "."

Item {
    id: power
    property bool popupOpen: false
    property int popupMonitorId: -1
    readonly property var actions: [
        { label: "Log out", subtitle: "End this Hyprland session", icon: "󰍃", command: ["hyprctl", "dispatch", "exit"] },
        { label: "Suspend", subtitle: "Keep this session ready", icon: "󰒲", command: ["systemctl", "suspend"] },
        { label: "Restart", subtitle: "Reboot the computer", icon: "󰜉", command: ["systemctl", "reboot"] },
        { label: "Shut down", subtitle: "Turn off the computer", icon: "󰐥", command: ["systemctl", "poweroff"] },
    ]

    function close() {
        popupOpen = false;
        popupMonitorId = -1;
    }

    function closeOtherPopups() {
        for (const target of ["network", "audio", "power", "notifications", "launcher"]) {
            if (target !== "power")
                Quickshell.execDetached(["quickshell", "ipc", "call", target, "close"]);
        }
    }

    function run(command) {
        close();
        Quickshell.execDetached(command);
    }

    IpcHandler {
        target: "power"
        function close(): void {
            power.close();
        }

        function toggle(): void {
            if (power.popupOpen) {
                power.close();
                return;
            }
            power.closeOtherPopups();
            if (!Hyprland.focusedMonitor) return;
            power.popupMonitorId = Hyprland.focusedMonitor.id;
            power.popupOpen = true;
        }
    }

    Variants {
        model: Quickshell.screens
        delegate: Component {
            PanelWindow {
                required property var modelData
                screen: modelData
                readonly property var monitor: Hyprland.monitorFor(screen)
                visible: power.popupOpen && monitor
                    && monitor.id === power.popupMonitorId
                color: Theme.transparent
                anchors { top: true; bottom: true; left: true; right: true }

                WlrLayershell.namespace: "hyprland-power"
                WlrLayershell.layer: WlrLayer.Overlay
                WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

                MouseArea {
                    anchors.fill: parent
                    onClicked: power.close()
                }

                Rectangle {
                    id: card
                    width: 320
                    height: 248
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.topMargin: 8
                    anchors.rightMargin: 14
                    radius: 12
                    color: Theme.surface
                    border.color: Theme.border
                    border.width: 1
                    focus: true
                    Keys.onEscapePressed: power.close()

                    MouseArea {
                        anchors.fill: parent
                        onClicked: mouse.accepted = true
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 6

                        Repeater {
                            model: power.actions
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: 50
                                radius: 8
                                color: actionMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised
                                border.color: actionMouse.containsMouse ? Theme.border : Theme.transparent
                                border.width: 1

                                Item {
                                    id: actionIcon
                                    anchors.left: parent.left
                                    anchors.leftMargin: 13
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 20
                                    height: 20

                                    AppText {
                                        anchors.centerIn: parent
                                        text: modelData.icon
                                        color: modelData.label === "Shut down" ? Theme.accent : Theme.text
                                        font.pixelSize: 20
                                    }
                                }
                                ColumnLayout {
                                    anchors.left: actionIcon.right
                                    anchors.right: parent.right
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 1
                                    AppText {
                                        Layout.fillWidth: true
                                        text: modelData.label
                                        color: Theme.text
                                        font.pixelSize: 13
                                        font.bold: true
                                    }
                                    AppText {
                                        Layout.fillWidth: true
                                        text: modelData.subtitle
                                        color: Theme.textDim
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }
                                MouseArea {
                                    id: actionMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: power.run(modelData.command)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
