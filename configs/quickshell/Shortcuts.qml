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
    property string searchText: ""
    readonly property var filteredRows: {
        const query = searchText.trim().toLowerCase();
        if (!query) return rows;
        return rows.filter(row =>
            (row.keys + " " + row.action).toLowerCase().includes(query));
    }

    IpcHandler {
        target: "shortcuts"
        function toggle(): void {
            shortcuts.popupOpen = !shortcuts.popupOpen;
            if (shortcuts.popupOpen) {
                shortcuts.searchText = "";
                binds.running = true;
            }
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
                visible: shortcuts.popupOpen && Hyprland.focusedMonitor
                    && monitor && Hyprland.focusedMonitor.id === monitor.id
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
                        radius: 12
                        color: Theme.surface
                        border.color: Theme.border
                        border.width: 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 22
                            spacing: 12

                            Item {
                                Layout.fillWidth: true
                                implicitHeight: 34

                                ColumnLayout {
                                    anchors.left: parent.left
                                    anchors.right: closeButton.left
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.rightMargin: 10
                                    spacing: 2

                                    Text {
                                        text: "Hyprland shortcuts"
                                        color: Theme.text
                                        font.pixelSize: 22
                                        font.bold: true
                                    }
                                    Text {
                                        text: "Live bindings from hyprctl"
                                        color: Theme.textDim
                                        font.pixelSize: 12
                                    }
                                }

                                Rectangle {
                                    id: closeButton
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    implicitWidth: 32
                                    implicitHeight: 32
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
                                        onClicked: shortcuts.popupOpen = false
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 40
                                radius: 8
                                color: searchField.activeFocus ? Theme.surfaceHover : Theme.surfaceRaised
                                border.color: searchField.activeFocus ? Theme.accent : Theme.border
                                border.width: 1

                                TextInput {
                                    id: searchField
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    verticalAlignment: TextInput.AlignVCenter
                                    color: Theme.text
                                    selectionColor: Theme.accent
                                    selectedTextColor: Theme.accentText
                                    focus: shortcuts.popupOpen && Hyprland.focusedMonitor
                                        && monitor && Hyprland.focusedMonitor.id === monitor.id
                                    onTextChanged: shortcuts.searchText = text
                                    Keys.onEscapePressed: shortcuts.popupOpen = false
                                }
                                Text {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: searchField.text.length === 0
                                    text: "Search shortcuts"
                                    color: Theme.textDim
                                    font: searchField.font
                                }
                            }

                            ListView {
                                id: list
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 2
                                model: shortcuts.filteredRows
                                delegate: Rectangle {
                                    required property var modelData
                                    required property int index
                                    width: list.width
                                    height: 36
                                    radius: 6
                                    color: index % 2 ? Theme.surfaceRaised : Theme.transparent

                                    Rectangle {
                                        id: keyBadge
                                        anchors.left: parent.left
                                        anchors.leftMargin: 8
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 210
                                        height: 26
                                        radius: 6
                                        color: Theme.surfaceHover

                                        Text {
                                            anchors.left: parent.left
                                            anchors.leftMargin: 8
                                            anchors.right: parent.right
                                            anchors.rightMargin: 8
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: modelData.keys
                                            color: Theme.accent
                                            font.family: "monospace"
                                            font.bold: true
                                            elide: Text.ElideRight
                                        }
                                    }
                                    Text {
                                        anchors.left: keyBadge.right
                                        anchors.leftMargin: 12
                                        anchors.right: parent.right
                                        anchors.rightMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: modelData.action
                                        color: Theme.text
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: list.count === 0
                                text: "No shortcuts found"
                                color: Theme.textDim
                                horizontalAlignment: Text.AlignHCenter
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
