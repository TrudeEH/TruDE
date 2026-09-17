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
    readonly property var actions: [
        { label: "Log out", subtitle: "End this Hyprland session", icon: "⇥", command: ["hyprctl", "dispatch", "exit"] },
        { label: "Suspend", subtitle: "Keep this session ready", icon: "◐", command: ["systemctl", "suspend"] },
        { label: "Restart", subtitle: "Reboot the computer", icon: "↻", command: ["systemctl", "reboot"] },
        { label: "Shut down", subtitle: "Turn off the computer", icon: "⏻", command: ["systemctl", "poweroff"] },
    ]

    function close() {
        popupOpen = false;
    }

    function run(command) {
        close();
        Quickshell.execDetached(command);
    }

    IpcHandler {
        target: "power"
        function toggle(): void {
            power.popupOpen = !power.popupOpen;
        }
    }

    Variants {
        model: Quickshell.screens
        delegate: Component {
            PanelWindow {
                required property var modelData
                screen: modelData
                readonly property var monitor: Hyprland.monitorFor(screen)
                visible: power.popupOpen && Hyprland.focusedMonitor
                    && monitor && Hyprland.focusedMonitor.id === monitor.id
                color: Theme.transparent
                implicitWidth: 320
                implicitHeight: 310
                anchors { top: true; right: true }
                margins { top: 44; right: 14 }

                WlrLayershell.namespace: "hyprland-power"
                WlrLayershell.layer: WlrLayer.Overlay
                WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

                Rectangle {
                    id: card
                    anchors.fill: parent
                    radius: 12
                    color: Theme.surface
                    border.color: Theme.border
                    border.width: 1
                    focus: true
                    Keys.onEscapePressed: power.close()

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 18
                        spacing: 10

                        Item {
                            Layout.fillWidth: true
                            implicitHeight: 34

                            ColumnLayout {
                                anchors.left: parent.left
                                anchors.right: closeButton.left
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2

                                Text {
                                    text: "Power"
                                    color: Theme.text
                                    font.pixelSize: 21
                                    font.bold: true
                                }
                                Text {
                                    text: "Choose a session action"
                                    color: Theme.textDim
                                    font.pixelSize: 12
                                }
                            }

                            Rectangle {
                                id: closeButton
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                width: 32
                                height: 32
                                radius: 16
                                color: closeMouse.containsMouse ? Theme.surfaceHover : Theme.transparent

                                Text {
                                    anchors.centerIn: parent
                                    text: "×"
                                    color: Theme.text
                                    font.pixelSize: 22
                                }
                                MouseArea {
                                    id: closeMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: power.close()
                                }
                            }
                        }

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

                                Text {
                                    id: actionIcon
                                    anchors.left: parent.left
                                    anchors.leftMargin: 13
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData.icon
                                    color: modelData.label === "Shut down" ? Theme.accent : Theme.text
                                    font.pixelSize: 20
                                }
                                ColumnLayout {
                                    anchors.left: actionIcon.right
                                    anchors.right: parent.right
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 1
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.label
                                        color: Theme.text
                                        font.pixelSize: 13
                                        font.bold: true
                                    }
                                    Text {
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
