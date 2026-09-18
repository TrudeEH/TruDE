import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import Quickshell.Wayland
import QtQuick
import QtQuick.Layouts
import "."

Item {
    id: maintenance

    property bool popupOpen: false
    property var popupScreen: null
    property string detailsText: "Collecting maintenance information..."
    readonly property var focusedScreen: {
        const focused = Hyprland.focusedMonitor;
        if (!focused) return null;
        for (const screen of Quickshell.screens) {
            if (screen.name === focused.name) return screen;
        }
        return null;
    }

    readonly property string detailsCommand:
        "printf '%s\\n' 'APT / DPKG'; printf 'Installed packages: '; dpkg-query -W -f='${binary:Package}\\n' 2>/dev/null | wc -l; printf 'Available upgrades: '; apt list --upgradable 2>/dev/null | tail -n +2 | sed '/^$/d' | wc -l; printf 'Backports installed: '; dpkg-query -W -f='${binary:Package} ${Version}\\n' 2>/dev/null | grep -E 'bpo|backport' | wc -l; printf 'Held packages: '; apt-mark showhold 2>/dev/null | sed '/^$/d' | wc -l; printf 'APT lists updated: '; stat -c '%y' /var/lib/apt/lists 2>/dev/null | cut -d. -f1 || printf 'unknown\\n'; printf '\\nUPGRADABLE PACKAGES\\n'; apt list --upgradable 2>/dev/null | sed '1{/^Listing/d;}' | head -n 30; printf '\\nBACKPORT PACKAGES\\n'; dpkg-query -W -f='${binary:Package} ${Version}\\n' 2>/dev/null | grep -E 'bpo|backport' | head -n 30 || printf '%s\\n' 'None detected'; "
        + "printf '\\nFLATPAK\\n'; if command -v flatpak >/dev/null 2>&1; then printf 'Installed apps: '; flatpak list --app --columns=application 2>/dev/null | sed '/^$/d' | wc -l; printf 'Available updates: '; flatpak remote-ls --updates --app 2>/dev/null | sed '/^$/d' | wc -l; printf '\\nFLATPAK UPDATES\\n'; flatpak remote-ls --updates --app 2>/dev/null | head -n 30 || true; else printf '%s\\n' 'Flatpak is not installed'; fi; "
        + "printf '\\nSYSTEM\\n'; if [ -e /var/run/reboot-required ]; then printf '%s\\n' 'Reboot required: yes'; else printf '%s\\n' 'Reboot required: no'; fi; printf 'Failed systemd units: '; systemctl --failed --no-legend 2>/dev/null | sed '/^$/d' | wc -l; systemctl --failed --no-legend 2>/dev/null | head -n 15"

    function close() {
        popupOpen = false;
        popupScreen = null;
    }

    function closeOtherPopups() {
        for (const target of ["network", "audio", "power", "notifications", "launcher", "system-monitor"])
            Quickshell.execDetached(["quickshell", "ipc", "call", target, "close"]);
    }

    function refresh() {
        detailsText = "Collecting maintenance information...";
        details.running = true;
    }

    function toggle() {
        if (popupOpen) {
            close();
            return;
        }
        closeOtherPopups();
        popupScreen = focusedScreen;
        popupOpen = popupScreen !== null;
        if (popupOpen) refresh();
    }

    IpcHandler {
        target: "maintenance"

        function toggle(): void {
            maintenance.toggle();
        }

        function close(): void {
            maintenance.close();
        }
    }

    Process {
        id: details
        command: ["sh", "-c", maintenance.detailsCommand]
        running: false
        stdout: StdioCollector {
            onStreamFinished: maintenance.detailsText = this.text.trim()
        }
    }

    PanelWindow {
        screen: maintenance.popupScreen
        visible: maintenance.popupOpen && maintenance.popupScreen !== null
        color: Theme.transparent
        anchors { top: true; bottom: true; left: true; right: true }

        WlrLayershell.namespace: "hyprland-maintenance"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

        MouseArea {
            anchors.fill: parent
            onClicked: maintenance.close()
        }

        Rectangle {
            id: card
            width: 540
            height: Math.min(parent.height - 24, 720)
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: 8
            anchors.rightMargin: 14
            radius: 12
            color: Theme.surface
            border.color: Theme.border
            border.width: 1
            focus: true

            Keys.onEscapePressed: maintenance.close()

            MouseArea {
                anchors.fill: parent
                onClicked: mouse.accepted = true
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        AppText {
                            text: "Debian maintenance"
                            color: Theme.text
                            font.pixelSize: 20
                            font.bold: true
                        }
                        AppText {
                            text: "Packages, updates, backports, and system health"
                            color: Theme.textDim
                            font.pixelSize: 12
                        }
                    }

                    Rectangle {
                        implicitWidth: 34
                        implicitHeight: 34
                        radius: 17
                        color: refreshMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised

                        AppText {
                            anchors.centerIn: parent
                            text: "󰑐"
                            color: Theme.text
                            font.pixelSize: 16
                        }
                        MouseArea {
                            id: refreshMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: maintenance.refresh()
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 8
                    color: Theme.window
                    border.color: Theme.border
                    border.width: 1

                    Flickable {
                        anchors.fill: parent
                        anchors.margins: 12
                        contentWidth: width
                        contentHeight: detailsLabel.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds

                        AppText {
                            id: detailsLabel
                            width: parent.width
                            text: maintenance.detailsText
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                        }
                    }
                }
            }
        }
    }
}
