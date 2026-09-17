import Quickshell
import Quickshell.Hyprland
import Quickshell.Services.SystemTray
import Quickshell.Wayland
import QtQuick
import QtQuick.Layouts

ShellRoot {
    SystemClock {
        id: clock
        precision: SystemClock.Minutes
    }

    // Create a Wayland layer-shell panel for each connected monitor.
    Variants {
        model: Quickshell.screens

        delegate: Component {
            PanelWindow {
                id: bar
                required property var modelData
                screen: modelData
                readonly property var hyprMonitor: Hyprland.monitorFor(screen)

                anchors { top: true; left: true; right: true }
                implicitHeight: 34
                exclusiveZone: implicitHeight
                color: "#1e1e2e"

                WlrLayershell.namespace: "hyprland-bar"
                WlrLayershell.layer: WlrLayer.Top
                WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 8

                    Rectangle {
                        width: 48
                        height: 24
                        radius: 4
                        color: launcherMouse.containsMouse ? "#45475a" : "#313244"

                        Text {
                            anchors.centerIn: parent
                            text: "Apps"
                            color: "#cdd6f4"
                            font.pixelSize: 12
                        }
                        MouseArea {
                            id: launcherMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Quickshell.execDetached(["fuzzel"])
                        }
                    }

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
                                color: active ? "#89b4fa" : (workspaceMouse.containsMouse ? "#45475a" : "#313244")

                                Text {
                                    anchors.centerIn: parent
                                    text: workspace.number === 10 ? "0" : workspace.number
                                    color: workspace.active ? "#1e1e2e" : "#cdd6f4"
                                    font.bold: workspace.active
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    id: workspaceMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    // Hyprland 0.55 uses Lua dispatchers.
                                    onClicked: Hyprland.dispatch(
                                        "hl.dsp.focus({ workspace = " + workspace.number + " })")
                                }
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }

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
                                color: trayMouse.containsMouse ? "#45475a" : "transparent"

                                Image {
                                    anchors.centerIn: parent
                                    width: 18
                                    height: 18
                                    source: Quickshell.iconPath(trayItem.modelData.icon, true)
                                    fillMode: Image.PreserveAspectFit
                                }
                                QsMenuAnchor {
                                    id: trayMenu
                                    menu: trayItem.modelData.menu
                                    anchor.item: trayItem
                                    anchor.edges: Edges.Bottom
                                    anchor.gravity: Edges.Bottom
                                }
                                MouseArea {
                                    id: trayMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                                    onClicked: mouse => {
                                        if (mouse.button === Qt.MiddleButton) {
                                            trayItem.modelData.secondaryActivate();
                                        } else if (mouse.button === Qt.RightButton || trayItem.modelData.onlyMenu) {
                                            if (trayItem.modelData.hasMenu) trayMenu.open();
                                        } else {
                                            trayItem.modelData.activate();
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        text: Qt.formatDateTime(clock.date, "ddd d MMM  HH:mm")
                        color: "#cdd6f4"
                        font.pixelSize: 12
                    }
                }
            }
        }
    }
}
