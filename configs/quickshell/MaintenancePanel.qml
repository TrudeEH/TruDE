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
    property string installedText: "—"
    property string aptUpdatesText: "—"
    property string backportsText: "—"
    property string heldText: "—"
    property string aptUpdatedText: "—"
    property string flatpakAppsText: "—"
    property string flatpakUpdatesText: "—"
    property string rebootText: "—"
    property string failedText: "—"
    property var upgrades: []
    property var backportPackages: []
    property var flatpakPackages: []
    property var failedUnits: []

    readonly property var focusedScreen: {
        const focused = Hyprland.focusedMonitor;
        if (!focused) return null;
        for (const screen of Quickshell.screens) {
            if (screen.name === focused.name) return screen;
        }
        return null;
    }

    readonly property string detailsCommand:
        "printf 'installed\\t%s\\n' \"$(dpkg-query -W -f='${binary:Package}\\n' 2>/dev/null | wc -l)\"; "
        + "printf 'aptUpdates\\t%s\\n' \"$(apt list --upgradable 2>/dev/null | tail -n +2 | sed '/^$/d' | wc -l)\"; "
        + "printf 'backports\\t%s\\n' \"$(dpkg-query -W -f='${binary:Package} ${Version}\\n' 2>/dev/null | grep -E 'bpo|backport' | wc -l)\"; "
        + "printf 'held\\t%s\\n' \"$(apt-mark showhold 2>/dev/null | sed '/^$/d' | wc -l)\"; "
        + "printf 'aptUpdated\\t%s\\n' \"$(stat -c '%y' /var/lib/apt/lists 2>/dev/null | cut -d. -f1 || printf 'unknown')\"; "
        + "printf 'flatpakApps\\t%s\\n' \"$(if command -v flatpak >/dev/null 2>&1; then flatpak list --app --columns=application 2>/dev/null | sed '/^$/d' | wc -l; else printf '0'; fi)\"; "
        + "printf 'flatpakUpdates\\t%s\\n' \"$(if command -v flatpak >/dev/null 2>&1; then flatpak remote-ls --updates --app 2>/dev/null | sed '/^$/d' | wc -l; else printf '0'; fi)\"; "
        + "if [ -e /var/run/reboot-required ]; then printf 'reboot\\tyes\\n'; else printf 'reboot\\tno\\n'; fi; "
        + "printf 'failed\\t%s\\n' \"$(systemctl --failed --no-legend 2>/dev/null | sed '/^$/d' | wc -l)\"; "
        + "apt list --upgradable 2>/dev/null | sed '1{/^Listing/d;}' | sed '/^$/d' | while read -r item; do printf 'upgrade\\t%s\\n' \"$item\"; done; "
        + "dpkg-query -W -f='${binary:Package} ${Version}\\n' 2>/dev/null | grep -E 'bpo|backport' | while read -r item; do printf 'backportPackage\\t%s\\n' \"$item\"; done; "
        + "if command -v flatpak >/dev/null 2>&1; then flatpak remote-ls --updates --app 2>/dev/null | sed '/^$/d' | while read -r item; do printf 'flatpakPackage\\t%s\\n' \"$item\"; done; fi; "
        + "systemctl --failed --no-legend 2>/dev/null | sed '/^$/d' | while read -r item; do printf 'failedUnit\\t%s\\n' \"$item\"; done"

    function close() {
        popupOpen = false;
        popupScreen = null;
    }

    function closeOtherPopups() {
        PopupManager.closeExcept("maintenance");
    }

    function applyDetails(output) {
        const parsedUpgrades = [];
        const parsedBackportPackages = [];
        const parsedFlatpakPackages = [];
        const parsedFailedUnits = [];

        for (const line of output.trim().split("\n")) {
            const separator = line.indexOf("\t");
            if (separator < 0) continue;
            const key = line.slice(0, separator);
            const value = line.slice(separator + 1).trim();
            if (key === "installed") installedText = value;
            else if (key === "aptUpdates") aptUpdatesText = value;
            else if (key === "backports") backportsText = value;
            else if (key === "held") heldText = value;
            else if (key === "aptUpdated") aptUpdatedText = value;
            else if (key === "flatpakApps") flatpakAppsText = value;
            else if (key === "flatpakUpdates") flatpakUpdatesText = value;
            else if (key === "reboot") rebootText = value;
            else if (key === "failed") failedText = value;
            else if (key === "upgrade") parsedUpgrades.push(value);
            else if (key === "backportPackage") parsedBackportPackages.push(value);
            else if (key === "flatpakPackage") parsedFlatpakPackages.push(value);
            else if (key === "failedUnit") parsedFailedUnits.push(value);
        }

        upgrades = parsedUpgrades;
        backportPackages = parsedBackportPackages;
        flatpakPackages = parsedFlatpakPackages;
        failedUnits = parsedFailedUnits;
    }

    function refresh() {
        installedText = "…";
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
        function toggle(): void { maintenance.toggle(); }
        function close(): void { maintenance.close(); }
    }

    Process {
        id: details
        command: ["sh", "-c", maintenance.detailsCommand]
        running: false
        stdout: StdioCollector {
            onStreamFinished: maintenance.applyDetails(this.text)
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
            width: 580
            height: Math.min(parent.height - 24, 760)
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
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Rectangle {
                        implicitWidth: 40
                        implicitHeight: 40
                        radius: 12
                        color: Theme.accent
                        AppText {
                            anchors.centerIn: parent
                            text: "󰏗"
                            color: Theme.accentText
                            font.pixelSize: 21
                        }
                    }

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

                Flickable {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: width
                    contentHeight: contentColumn.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    ColumnLayout {
                        id: contentColumn
                        width: parent.width
                        spacing: 14

                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 8
                            rowSpacing: 8

                            InfoCard { Layout.fillWidth: true; icon: "󰏗"; title: "Installed"; value: maintenance.installedText; detail: "APT packages" }
                            InfoCard { Layout.fillWidth: true; icon: "󰏓"; title: "Updates"; value: maintenance.aptUpdatesText; detail: "APT upgrades"; highlighted: Number(maintenance.aptUpdatesText) > 0 }
                            InfoCard { Layout.fillWidth: true; icon: "󰒓"; title: "Backports"; value: maintenance.backportsText; detail: "Backport packages" }
                            InfoCard { Layout.fillWidth: true; icon: "󰈇"; title: "Held"; value: maintenance.heldText; detail: "Pinned packages" }
                            InfoCard { Layout.fillWidth: true; icon: "󰏖"; title: "Flatpak apps"; value: maintenance.flatpakAppsText; detail: "Installed apps" }
                            InfoCard { Layout.fillWidth: true; icon: "󰚰"; title: "Flatpak updates"; value: maintenance.flatpakUpdatesText; detail: "Available updates"; highlighted: Number(maintenance.flatpakUpdatesText) > 0 }
                            InfoCard { Layout.fillWidth: true; icon: "󰁹"; title: "Reboot"; value: maintenance.rebootText; detail: "Kernel/system state"; highlighted: maintenance.rebootText === "yes" }
                            InfoCard { Layout.fillWidth: true; icon: "󰒓"; title: "Failed units"; value: maintenance.failedText; detail: "systemd services"; highlighted: Number(maintenance.failedText) > 0 }
                        }

                        AppText {
                            text: "APT STATUS"
                            color: Theme.textDim
                            font.pixelSize: 11
                            font.bold: true
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 42
                            radius: 9
                            color: Theme.surfaceRaised
                            border.color: Theme.border
                            border.width: 1
                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                AppText { text: "󰅐"; color: Theme.accent; font.pixelSize: 15 }
                                AppText { text: "Package lists updated"; color: Theme.textSecondary; font.pixelSize: 12; Layout.fillWidth: true }
                                AppText { text: maintenance.aptUpdatedText; color: Theme.text; font.pixelSize: 11 }
                            }
                        }

                        AppText { text: "UPGRADABLE PACKAGES"; color: Theme.textDim; font.pixelSize: 11; font.bold: true }
                        Repeater {
                            model: maintenance.upgrades.slice(0, 20)
                            delegate: Rectangle {
                                required property string modelData
                                Layout.fillWidth: true
                                implicitHeight: 34
                                radius: 8
                                color: packageMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised
                                border.color: packageMouse.containsMouse ? Theme.border : Theme.transparent
                                border.width: 1
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    AppText { text: "󰏗"; color: Theme.accent; font.pixelSize: 13 }
                                    AppText { text: modelData; color: Theme.textSecondary; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideRight }
                                }
                                MouseArea { id: packageMouse; anchors.fill: parent; hoverEnabled: true }
                            }
                        }

                        AppText { visible: maintenance.upgrades.length === 0; text: "No APT upgrades available"; color: Theme.textDim; font.pixelSize: 12 }

                        AppText { visible: maintenance.backportPackages.length > 0; text: "BACKPORT PACKAGES"; color: Theme.textDim; font.pixelSize: 11; font.bold: true }
                        Repeater {
                            model: maintenance.backportPackages.slice(0, 20)
                            delegate: Rectangle {
                                required property string modelData
                                Layout.fillWidth: true
                                implicitHeight: 34
                                radius: 8
                                color: Theme.surfaceRaised
                                border.color: Theme.border
                                border.width: 1
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    AppText { text: "󰒓"; color: Theme.accent; font.pixelSize: 13 }
                                    AppText { text: modelData; color: Theme.textSecondary; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideRight }
                                }
                            }
                        }

                        AppText { visible: maintenance.flatpakPackages.length > 0; text: "FLATPAK UPDATES"; color: Theme.textDim; font.pixelSize: 11; font.bold: true }
                        Repeater {
                            model: maintenance.flatpakPackages.slice(0, 20)
                            delegate: Rectangle {
                                required property string modelData
                                Layout.fillWidth: true
                                implicitHeight: 34
                                radius: 8
                                color: Theme.surfaceRaised
                                border.color: Theme.border
                                border.width: 1
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    AppText { text: "󰏖"; color: Theme.accent; font.pixelSize: 13 }
                                    AppText { text: modelData; color: Theme.textSecondary; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideRight }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
