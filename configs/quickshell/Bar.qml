import Quickshell
import Quickshell.Bluetooth
import Quickshell.Hyprland
import Quickshell.Io
import Quickshell.Networking
import Quickshell.Services.Pipewire
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
    property string currentProfile: "balanced"
    property bool batteryPresent: false
    property int batteryPercent: 0
    property int maintenanceUpdates: 0
    readonly property var hyprMonitor: Hyprland.monitorFor(screen)
    readonly property var networkDevices: Networking.devices.values
    readonly property var connectedNetworkDevice: findConnectedNetworkDevice()
    readonly property var connectedNetwork: findConnectedNetwork(connectedNetworkDevice)
    readonly property var bluetoothAdapter: Bluetooth.defaultAdapter
    readonly property var bluetoothDevices: Bluetooth.devices.values
    readonly property var connectedBluetoothDevices: bluetoothDevices.filter(device => device.connected)
    readonly property var audioNodes: Pipewire.nodes.values
    readonly property var audioDevices: audioNodes.filter(node => node.audio && !node.isStream)
    readonly property var defaultSink: Pipewire.defaultAudioSink
    readonly property int volumePercent: defaultSink && defaultSink.audio
        ? Math.round(defaultSink.audio.volume * 100) : 0
    readonly property string volumeIcon: {
        if (!defaultSink || !defaultSink.audio || defaultSink.audio.muted) return "󰖁";
        if (volumePercent <= 0) return "󰕿";
        if (volumePercent < 35) return "󰕿";
        if (volumePercent < 70) return "󰖀";
        return "󰕾";
    }
    readonly property string bluetoothIcon: bluetoothAdapter && bluetoothAdapter.enabled
        ? (connectedBluetoothDevices.length > 0 ? "󰂱" : "󰂯") : "󰂲"
    readonly property string networkIcon: {
        if (!connectedNetworkDevice) return "󰤭";
        if (connectedNetworkDevice.type === DeviceType.Wired) return "󰈀";
        if (!connectedNetwork) return "󰤭";
        const strength = connectedNetwork.signalStrength || 0;
        return strength < 0.25 ? "󰤟" : strength < 0.5 ? "󰤢"
            : strength < 0.75 ? "󰤥" : "󰤨";
    }
    readonly property string profileIcon: currentProfile === "performance" ? "󰓅"
        : currentProfile === "power-saver" ? "󰌪" : "󰾅"
    property var occupiedWorkspaces: []
    readonly property var physicalWorkspaceIds: {
        if (Hyprland.monitors.values.length <= 1) return [];
        const ids = [];
        for (const monitor of Hyprland.monitors.values) {
            if (monitor.activeWorkspace) ids.push(monitor.activeWorkspace.id);
        }
        return ids;
    }

    function findConnectedNetworkDevice() {
        for (const device of networkDevices) {
            if (device.connected) return device;
        }
        return null;
    }

    function findConnectedNetwork(device) {
        if (!device) return null;
        if (device.type === DeviceType.Wired) return device.network || null;
        for (const network of device.networks.values) {
            if (network.connected) return network;
        }
        return null;
    }

    function adjustVolume(steps) {
        if (!defaultSink || !defaultSink.audio) return;
        if (defaultSink.audio.muted && steps > 0) defaultSink.audio.muted = false;
        defaultSink.audio.volume = Math.max(0, Math.min(1.5,
            defaultSink.audio.volume + steps * 0.05));
    }

    Process {
        id: workspaceState
        command: ["sh", "-c", "hyprctl workspaces -j"]
        running: false
        stdout: StdioCollector {
            onStreamFinished: {
                try {
                    bar.occupiedWorkspaces = JSON.parse(this.text).map(workspace => workspace.id);
                } catch (error) {
                    bar.occupiedWorkspaces = [];
                }
            }
        }
    }

    Process {
        id: profileQuery
        command: ["powerprofilesctl", "get"]
        running: false
        stdout: StdioCollector {
            onStreamFinished: {
                const profile = this.text.trim();
                if (["power-saver", "balanced", "performance"].indexOf(profile) >= 0)
                    bar.currentProfile = profile;
            }
        }
    }

    Process {
        id: batteryQuery
        command: ["sh", "-c", "battery=$(upower -e 2>/dev/null | awk '/\\/battery_/ { print; exit }'); [ -n \"$battery\" ] || exit 0; info=$(upower -i \"$battery\" 2>/dev/null); present=$(printf '%s\\n' \"$info\" | sed -n 's/^[[:space:]]*present:[[:space:]]*//p' | head -n 1); [ \"${present:-yes}\" = yes ] || exit 0; percentage=$(printf '%s\\n' \"$info\" | sed -n 's/^[[:space:]]*percentage:[[:space:]]*\\([0-9][0-9]*\\)%.*/\\1/p' | head -n 1); [ -n \"$percentage\" ] && printf '%s\\n' \"$percentage\""]
        running: false
        stdout: StdioCollector {
            onStreamFinished: {
                const percentage = Number(this.text.trim());
                bar.batteryPresent = this.text.trim() !== "" && !isNaN(percentage);
                bar.batteryPercent = bar.batteryPresent ? percentage : 0;
            }
        }
    }

    Process {
        id: maintenanceQuery
        command: ["sh", "-c", "apt_updates=$(apt list --upgradable 2>/dev/null | sed '1{/^Listing/d;}; /^$/d' | wc -l); flatpak_updates=$(if command -v flatpak >/dev/null 2>&1; then flatpak remote-ls --updates --app 2>/dev/null | sed '/^$/d' | wc -l; else printf '0'; fi); printf '%s\\n' $((apt_updates + flatpak_updates))"]
        running: false
        stdout: StdioCollector {
            onStreamFinished: bar.maintenanceUpdates = Number(this.text.trim()) || 0
        }
    }

    Timer {
        interval: 2000
        running: true
        repeat: true
        onTriggered: workspaceState.running = true
    }

    Timer {
        interval: 3000
        running: true
        repeat: true
        onTriggered: if (!profileQuery.running) profileQuery.running = true
    }

    Timer {
        interval: 15000
        running: true
        repeat: true
        onTriggered: if (!batteryQuery.running) batteryQuery.running = true
    }

    Timer {
        interval: 30000
        running: true
        repeat: true
        onTriggered: if (!maintenanceQuery.running) maintenanceQuery.running = true
    }

    Component.onCompleted: {
        workspaceState.running = true;
        profileQuery.running = true;
        batteryQuery.running = true;
        maintenanceQuery.running = true;
    }

    PwObjectTracker { objects: bar.audioDevices }

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

        RowLayout {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            LauncherButton { Layout.alignment: Qt.AlignVCenter }

            Row {
                Layout.alignment: Qt.AlignVCenter
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
                        readonly property bool occupied: bar.occupiedWorkspaces.indexOf(number) >= 0
                        readonly property bool physical: bar.physicalWorkspaceIds.indexOf(number) >= 0

                        Layout.alignment: Qt.AlignVCenter
                        width: 24
                        height: 24
                        radius: 4
                        color: active ? Theme.accent : (workspaceMouse.containsMouse ? Theme.surfaceHover : Theme.surface)
                        border.color: occupied && !active ? Theme.accent : Theme.transparent
                        border.width: occupied && !active ? 2 : 1

                        AppText {
                            anchors.centerIn: parent
                            text: workspace.physical && !workspace.active
                                ? "󰍹" : (workspace.number === 10 ? "0" : workspace.number)
                            color: workspace.active ? Theme.accentText : Theme.text
                            font.bold: workspace.active
                            font.pixelSize: workspace.physical && !workspace.active ? 14 : 12
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
                    text: Qt.formatDateTime(clock.date, "ddd d MMM  h:mm AP")
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

        RowLayout {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            SystemMonitorButton { Layout.alignment: Qt.AlignVCenter }

            RowLayout {
                Layout.alignment: Qt.AlignVCenter
                spacing: 4

                TuiLauncherButton {
                    icon: bar.volumeIcon
                    label: bar.volumePercent + "%"
                    command: [
                        "foot",
                        "--app-id=pulsemixer",
                        "--title=Volume Mixer",
                        "--window-size-chars=94x28",
                        "pulsemixer"
                    ]
                    onScrolled: steps => bar.adjustVolume(steps)
                }

                TuiLauncherButton {
                    icon: bar.bluetoothIcon
                    label: bar.connectedBluetoothDevices.length > 0
                        ? String(bar.connectedBluetoothDevices.length) : ""
                    command: [
                        "foot",
                        "--app-id=bluetooth-tui",
                        "--title=Bluetooth",
                        "--override=colors.regular0=222226",
                        "--window-size-chars=82x26",
                        "dotfiles-bluetooth-tui"
                    ]
                }

                TuiLauncherButton {
                    icon: bar.networkIcon
                    command: [
                        "foot",
                        "--app-id=nmtui",
                        "--title=Network Settings",
                        "--override=colors.regular0=222226",
                        "--window-size-chars=82x24",
                        "dotfiles-network-tui"
                    ]
                }

                TuiLauncherButton {
                    icon: bar.profileIcon
                    label: bar.batteryPresent ? bar.batteryPercent + "%" : ""
                    command: [
                        "foot",
                        "--app-id=power-profiles-tui",
                        "--title=Power Profile",
                        "--override=colors.regular0=222226",
                        "--window-size-chars=82x24",
                        "dotfiles-power-profiles-tui"
                    ]
                }

                TuiLauncherButton {
                    icon: "󰐥"
                    command: [
                        "foot",
                        "--app-id=power-menu-tui",
                        "--title=Power",
                        "--override=colors.regular0=222226",
                        "--window-size-chars=88x23",
                        "dotfiles-power-menu-tui"
                    ]
                }
            }

            TuiLauncherButton {
                Layout.alignment: Qt.AlignVCenter
                icon: "󰏗"
                label: bar.maintenanceUpdates > 0
                    ? String(bar.maintenanceUpdates) : ""
                command: [
                    "foot",
                    "--app-id=maintenance-tui",
                    "--title=Debian Maintenance",
                    "--override=colors.regular0=222226",
                    "--window-size-chars=104x32",
                    "dotfiles-maintenance-tui"
                ]
            }

            Row {
                Layout.alignment: Qt.AlignVCenter
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
                                    PopupManager.closeExcept("tray");
                                    trayMenu.open = true;
                                } else {
                                    trayItem.modelData.activate();
                                }
                            }
                        }
                    }
                }
            }

            ScreenSharingIndicator { Layout.alignment: Qt.AlignVCenter }
        }
    }
}
