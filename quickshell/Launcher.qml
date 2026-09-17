import Quickshell
import Quickshell.Hyprland
import Quickshell.Wayland
import QtQuick
import QtQuick.Layouts
import "."

Item {
    id: launcher

    property bool popupOpen: false
    property string searchText: ""
    readonly property var entries: DesktopEntries.applications.values
    readonly property var focusedScreen: {
        const focused = Hyprland.focusedMonitor;
        if (!focused) return null;
        for (const screen of Quickshell.screens) {
            if (screen.name === focused.name) return screen;
        }
        return null;
    }
    readonly property var filteredEntries: {
        const query = searchText.trim().toLowerCase();
        return entries
            .filter(entry => !entry.noDisplay
                && (!query || (entry.name + " " + entry.genericName + " "
                    + entry.comment + " " + entry.keywords.join(" "))
                    .toLowerCase().includes(query)))
            .sort((a, b) => a.name.localeCompare(b.name));
    }

    function close() {
        popupOpen = false;
        searchText = "";
    }

    function toggle() {
        if (popupOpen) close();
        else {
            popupOpen = true;
            searchText = "";
        }
    }

    function launch(entry) {
        entry.execute();
        close();
    }

    IpcHandler {
        target: "launcher"
        function toggle(): void {
            launcher.toggle();
        }
    }

    PanelWindow {
        id: launcherWindow
        screen: launcher.focusedScreen
        visible: launcher.popupOpen && launcher.focusedScreen !== null
        color: Theme.transparent
        anchors { top: true; bottom: true; left: true; right: true }

        WlrLayershell.namespace: "hyprland-launcher"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.Exclusive

        Rectangle {
            anchors.fill: parent
            color: Theme.overlay

            MouseArea {
                anchors.fill: parent
                onClicked: launcher.close()
            }

            Rectangle {
                id: card
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 620)
                height: Math.min(parent.height - 48, 560)
                radius: 14
                color: Theme.surface
                border.color: Theme.border
                border.width: 1

                MouseArea {
                    anchors.fill: parent
                    onClicked: mouse.accepted = true
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 12

                    Item {
                        Layout.fillWidth: true
                        implicitHeight: 34

                        Text {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Applications"
                            color: Theme.text
                            font.pixelSize: 22
                            font.bold: true
                        }

                        Text {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: launcher.filteredEntries.length + " apps"
                            color: Theme.textDim
                            font.pixelSize: 12
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 44
                        radius: 10
                        color: searchField.activeFocus ? Theme.surfaceHover : Theme.surfaceRaised
                        border.color: searchField.activeFocus ? Theme.accent : Theme.border
                        border.width: 1

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            text: "⌕"
                            color: Theme.textDim
                            font.pixelSize: 21
                        }

                        TextInput {
                            id: searchField
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 42
                            anchors.rightMargin: 14
                            color: Theme.text
                            selectionColor: Theme.accent
                            selectedTextColor: Theme.accentText
                            font.pixelSize: 14
                            focus: launcherWindow.visible
                            onTextChanged: launcher.searchText = text
                            Keys.onEscapePressed: launcher.close()
                            Keys.onReturnPressed: {
                                if (launcher.filteredEntries.length > 0)
                                    launcher.launch(launcher.filteredEntries[0]);
                            }
                        }

                        Text {
                            anchors.left: searchField.left
                            anchors.verticalCenter: searchField.verticalCenter
                            visible: searchField.text.length === 0
                            text: "Search applications"
                            color: Theme.textDim
                            font: searchField.font
                        }
                    }

                    ListView {
                        id: list
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 3
                        model: launcher.filteredEntries

                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            width: list.width
                            height: 58
                            radius: 9
                            color: itemMouse.containsMouse ? Theme.surfaceHover : Theme.transparent

                            Image {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                width: 36
                                height: 36
                                source: Quickshell.iconPath(modelData.icon, true)
                                fillMode: Image.PreserveAspectFit
                                smooth: true
                            }

                            ColumnLayout {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: 58
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.name
                                    color: Theme.text
                                    font.pixelSize: 14
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    visible: text.length > 0
                                    text: modelData.genericName || modelData.comment || ""
                                    color: Theme.textDim
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }

                            MouseArea {
                                id: itemMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: launcher.launch(modelData)
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: list.count === 0
                        text: launcher.entries.length === 0
                            ? "No applications found"
                            : "No matching applications"
                        color: Theme.textDim
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }
        }
    }
}
