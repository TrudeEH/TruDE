import Quickshell
import Quickshell.Wayland
import QtQuick
import "."

Item {
    id: root

    required property var trayItem
    required property var anchorItem
    required property var barRoot
    required property var barWindow
    property bool open: false

    function toggle() {
        open = !open;
    }

    function close() {
        open = false;
    }

    function activateEntry(entry) {
        if (typeof entry.sendTriggered === "function") {
            entry.sendTriggered();
        } else if (typeof entry.triggered === "function") {
            entry.triggered();
        }
        close();
    }

    PanelWindow {
        id: popup
        screen: root.barWindow.screen
        visible: root.open
        color: Theme.transparent
        anchors { top: true; bottom: true; left: true; right: true }

        WlrLayershell.namespace: "hyprland-tray-menu"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

        MouseArea {
            anchors.fill: parent
            onClicked: root.close()
        }

        Rectangle {
            id: card
            x: Math.max(10, Math.min(parent.width - width - 10,
                root.barRoot.x + root.anchorItem.mapToItem(root.barRoot, 0, 0).x
                + root.anchorItem.width - width))
            y: 9
            width: menuView.implicitWidth + 16
            height: menuView.implicitHeight + 16
            radius: 10
            color: Theme.surface
            border.color: Theme.border
            border.width: 1

            MouseArea {
                anchors.fill: parent
                onClicked: mouse.accepted = true
            }

            TrayMenuView {
                id: menuView
                anchors.fill: parent
                anchors.margins: 8
                menu: root.trayItem.menu
                activateEntry: root.activateEntry
            }

            focus: true
            Keys.onEscapePressed: root.close()
        }
    }
}
