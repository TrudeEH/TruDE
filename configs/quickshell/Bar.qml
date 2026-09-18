import Quickshell
import Quickshell.Hyprland
import Quickshell.Services.SystemTray
import Quickshell.Wayland
import Quickshell.Widgets
import QtQuick
import QtQuick.Layouts
import "."

PanelWindow {
    id: bar
    required property var modelData
    required property var notificationCenter
    screen: modelData
    readonly property var hyprMonitor: Hyprland.monitorFor(screen)

    SystemClock {
        id: clock
        precision: SystemClock.Minutes
    }

    anchors { top: true; left: true; right: true }
    implicitHeight: 34
    exclusiveZone: implicitHeight
    color: Theme.window

    WlrLayershell.namespace: "hyprland-bar"
    WlrLayershell.layer: WlrLayer.Top
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

    Item {
        id: barContent
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10

        Row {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            LauncherButton {}

            Row {
                spacing: 4
                Repeater {
                    model: 10
                    delegate: Rectangle {
                        id: workspace
                        required property int index
                        readonly property int number: index + 1
                        readonly property bool active: bar.hyprMonitor
                            && bar.hyprMonitor.activeWorkspace
                            && bar.hyprMonitor.activeWorkspace.id === number

                        width: 24
                        height: 24
                        radius: 4
                        color: active ? Theme.accent : (workspaceMouse.containsMouse ? Theme.surfaceHover : Theme.surface)

                        AppText {
                            anchors.centerIn: parent
                            text: workspace.number === 10 ? "0" : workspace.number
                            color: workspace.active ? Theme.accentText : Theme.text
                            font.bold: workspace.active
                            font.pixelSize: 12
                        }
                        MouseArea {
                            id: workspaceMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Hyprland.dispatch(
                                "hl.dsp.focus({ workspace = " + workspace.number + " })")
                        }
                    }
                }
            }
        }

        Rectangle {
            id: clockBadge
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: clockContent.implicitWidth + 18
            height: 26
            radius: 8
            color: clockMouse.containsMouse ? Theme.surfaceHover : Theme.window
            border.color: Theme.border
            border.width: 1
            z: 2

            RowLayout {
                id: clockContent
                anchors.centerIn: parent
                spacing: 8

                AppText {
                    id: clockText
                    text: Qt.formatDateTime(clock.date, "ddd d MMM  HH:mm")
                    color: Theme.text
                    font.pixelSize: 12
                }

                AppText {
                    visible: bar.notificationCenter.count > 0
                    text: "󰂚 " + bar.notificationCenter.count
                    color: Theme.accent
                    font.pixelSize: 12
                }
            }

            MouseArea {
                id: clockMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: bar.notificationCenter.toggle()
            }
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            SystemMonitorButton {}

            NetworkButton {}

            AudioButton {}

            Row {
                spacing: 4
                Repeater {
                    model: SystemTray.items
                    delegate: Rectangle {
                        id: trayItem
                        required property var modelData
                        width: 24
                        height: 24
                        radius: 4
                        color: trayMouse.containsMouse ? Theme.surfaceHover : Theme.transparent

                        IconImage {
                            anchors.centerIn: parent
                            width: 18
                            height: 18
                            source: trayItem.modelData.icon
                            mipmap: true
                        }
                        TrayMenu {
                            id: trayMenu
                            trayItem: trayItem.modelData
                            anchorItem: trayItem
                            barRoot: barContent
                            barWindow: bar
                        }
                        MouseArea {
                            id: trayMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                            onClicked: mouse => {
                                if (mouse.button === Qt.MiddleButton) {
                                    trayItem.modelData.secondaryActivate();
                                } else if (trayItem.modelData.hasMenu
                                    && (mouse.button === Qt.RightButton || trayItem.modelData.onlyMenu)) {
                                    trayMenu.open = true;
                                } else {
                                    trayItem.modelData.activate();
                                }
                            }
                        }
                    }
                }
            }

            PowerButton {}
        }
    }
}
