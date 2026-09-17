import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import Quickshell.Wayland
import QtQuick
import QtQuick.Layouts
import "."

Item {
    id: shortcuts
    property bool popupOpen: false
    property var rows: []

    IpcHandler {
        target: "shortcuts"
        function toggle(): void {
            shortcuts.popupOpen = !shortcuts.popupOpen;
            if (shortcuts.popupOpen) binds.running = true;
        }
    }

    Process {
        id: binds
        command: ["hyprctl", "binds", "-j"]
        running: false
        stdout: StdioCollector {
            onStreamFinished: {
                try {
                    rows = JSON.parse(this.text).map(bind => ({
                        keys: formatKeys(bind),
                        action: bind.description || formatAction(bind),
                    }));
                } catch (error) {
                    rows = [{ keys: "", action: "Could not read Hyprland bindings" }];
                }
            }
        }
    }

    function formatKeys(bind) {
        const modifiers = [];
        if (bind.modmask & 64) modifiers.push("Super");
        if (bind.modmask & 8) modifiers.push("Alt");
        if (bind.modmask & 4) modifiers.push("Ctrl");
        if (bind.modmask & 1) modifiers.push("Shift");
        return modifiers.concat(bind.key || "(mouse)").join("+");
    }

    function formatAction(bind) {
        if (bind.dispatcher === "__lua") return "Configured Lua action (" + bind.arg + ")";
        return bind.dispatcher + (bind.arg ? "  " + bind.arg : "");
    }

    Variants {
        model: Quickshell.screens
        delegate: Component {
            PanelWindow {
                required property var modelData
                screen: modelData
                readonly property var monitor: Hyprland.monitorFor(screen)
                visible: shortcuts.popupOpen
                color: Theme.transparent
                anchors { top: true; bottom: true; left: true; right: true }

                WlrLayershell.namespace: "hyprland-shortcuts"
                WlrLayershell.layer: WlrLayer.Overlay
                WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

                Rectangle {
                    anchors.fill: parent
                    color: Theme.overlay

                    Rectangle {
                        id: card
                        anchors.centerIn: parent
                        width: Math.min(parent.width - 80, 820)
                        height: Math.min(parent.height - 80, 720)
                        radius: 10
                        color: Theme.surface
                        border.color: Theme.border
                        border.width: 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 22
                            spacing: 12

                            Text {
                                text: "Hyprland shortcuts"
                                color: Theme.text
                                font.pixelSize: 22
                                font.bold: true
                            }
                            Text {
                                text: "Live bindings from hyprctl · Escape closes"
                                color: Theme.textDim
                                font.pixelSize: 12
                            }
                            ListView {
                                id: list
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 2
                                model: shortcuts.rows
                                delegate: Rectangle {
                                    required property var modelData
                                    required property int index
                                    width: list.width
                                    height: 32
                                    radius: 4
                                    color: index % 2 ? Theme.surfaceRaised : Theme.window
                                    Text {
                                        anchors.left: parent.left
                                        anchors.leftMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 220
                                        text: modelData.keys
                                        color: Theme.accent
                                        font.family: "monospace"
                                        font.bold: true
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        anchors.left: parent.left
                                        anchors.leftMargin: 240
                                        anchors.right: parent.right
                                        anchors.rightMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: modelData.action
                                        color: Theme.text
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                        focus: true
                        Keys.onEscapePressed: shortcuts.popupOpen = false
                    }
                }
            }
        }
    }
}
