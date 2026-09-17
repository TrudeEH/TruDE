import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import Quickshell.Wayland
import QtQuick
import QtQuick.Layouts
import "."

Item {
    id: launcher

    property bool popupOpen: false
    property string searchText: ""
    property int selectedIndex: 0
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
            selectedIndex = 0;
        }
    }

    function launch(entry) {
        entry.execute();
        close();
    }

    function moveSelection(direction) {
        if (filteredEntries.length === 0) return;
        selectedIndex = (selectedIndex + direction + filteredEntries.length)
            % filteredEntries.length;
        list.positionViewAtIndex(selectedIndex, ListView.Contain);
    }

    function launchSelection() {
        if (filteredEntries.length > 0)
            launch(filteredEntries[selectedIndex]);
    }

    onSearchTextChanged: selectedIndex = 0

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

                        AppText {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Applications"
                            color: Theme.text
                            font.pixelSize: 22
                            font.bold: true
                        }

                        AppText {
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

                        AppText {
                            anchors.left: parent.left
                            anchors.leftMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            text: "󰍉"
                            color: Theme.textDim
                            font.pixelSize: 21
                        }

                        TextInput {
                            id: searchField
                            text: launcher.searchText
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 42
                            anchors.rightMargin: 14
                            color: Theme.text
                            selectionColor: Theme.accent
                            selectedTextColor: Theme.accentText
                            font.pixelSize: 14
                            font.family: Theme.fontFamily
                            focus: launcherWindow.visible
                            onTextEdited: launcher.searchText = text
                            Keys.onEscapePressed: launcher.close()
                            Keys.onDownPressed: launcher.moveSelection(1)
                            Keys.onUpPressed: launcher.moveSelection(-1)
                            Keys.onReturnPressed: launcher.launchSelection()
                        }

                        AppText {
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
                        currentIndex: launcher.selectedIndex

                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            width: list.width
                            height: 58
                            radius: 9
                            color: index === launcher.selectedIndex || itemMouse.containsMouse
                                ? Theme.surfaceHover : Theme.transparent

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

                                AppText {
                                    Layout.fillWidth: true
                                    text: modelData.name
                                    color: Theme.text
                                    font.pixelSize: 14
                                    elide: Text.ElideRight
                                }
                                AppText {
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
                                onEntered: launcher.selectedIndex = index
                                onClicked: launcher.launch(modelData)
                            }
                        }
                    }

                    AppText {
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
