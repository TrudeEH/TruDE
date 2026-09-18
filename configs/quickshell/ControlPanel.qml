import Quickshell
import Quickshell.Bluetooth
import Quickshell.Io
import Quickshell.Networking
import Quickshell.Services.Pipewire
import Quickshell.Wayland
import QtQuick
import QtQuick.Layouts
import "."

Rectangle {
    id: control

    required property var panelScreen

    property bool popupOpen: false
    property string expandedSection: ""
    property bool powerMenuOpen: false
    property var selectedNetwork: null
    property string wifiPassword: ""
    property string networkError: ""
    property bool profilesAvailable: false
    property string currentProfile: "balanced"
    property var availableProfiles: []

    readonly property var networkDevices: Networking.devices.values
    readonly property var wifiDevice: findWifiDevice()
    readonly property var connectedNetworkDevice: findConnectedNetworkDevice()
    readonly property var connectedNetwork: findConnectedNetwork(connectedNetworkDevice)
    readonly property var bluetoothAdapter: Bluetooth.defaultAdapter
    readonly property var bluetoothDevices: Bluetooth.devices.values
    readonly property var connectedBluetoothDevices: bluetoothDevices.filter(device => device.connected)
    readonly property var audioNodes: Pipewire.nodes.values
    readonly property var audioDevices: audioNodes.filter(node => node.audio && node.ready && !node.isStream)
    readonly property var outputDevices: audioDevices.filter(node => node.isSink)
    readonly property var defaultSink: Pipewire.defaultAudioSink
    readonly property int volumePercent: defaultSink && defaultSink.audio
        ? Math.round(defaultSink.audio.volume * 100) : 0
    readonly property string networkIcon: {
        if (!connectedNetworkDevice) return "󰤭";
        if (connectedNetworkDevice.type === DeviceType.Wired) return "󰈀";
        if (!connectedNetwork) return "󰤨";
        const strength = connectedNetwork.signalStrength || 0;
        return strength < 0.25 ? "󰤟" : strength < 0.5 ? "󰤢" : strength < 0.75 ? "󰤥" : "󰤨";
    }
    readonly property string bluetoothIcon: bluetoothAdapter && bluetoothAdapter.enabled
        ? (connectedBluetoothDevices.length > 0 ? "󰂱" : "󰂯") : "󰂲"
    readonly property string volumeIcon: !defaultSink || !defaultSink.audio || defaultSink.audio.muted
        ? "󰖁" : volumePercent === 0 ? "󰕿" : volumePercent < 50 ? "󰖀" : "󰕾"
    readonly property string profileIcon: currentProfile === "performance" ? "󰓅"
        : currentProfile === "power-saver" ? "󰌪" : "󰾅"
    readonly property string networkSubtitle: {
        if (!connectedNetworkDevice) return Networking.wifiEnabled ? "Not connected" : "Wi-Fi off";
        if (connectedNetwork && connectedNetwork.name) return connectedNetwork.name;
        return connectedNetworkDevice.type === DeviceType.Wired ? "Wired" : "Connected";
    }
    readonly property string bluetoothSubtitle: {
        if (!bluetoothAdapter) return "Unavailable";
        if (!bluetoothAdapter.enabled) return "Off";
        if (connectedBluetoothDevices.length === 0) return "On";
        if (connectedBluetoothDevices.length === 1) return connectedBluetoothDevices[0].name;
        return connectedBluetoothDevices.length + " devices connected";
    }
    readonly property string profileLabel: currentProfile === "power-saver" ? "Power Saver"
        : currentProfile === "performance" ? "Performance" : "Balanced"

    function findWifiDevice() {
        for (const device of networkDevices) {
            if (device.type === DeviceType.Wifi) return device;
        }
        return null;
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

    function setExpanded(section) {
        expandedSection = expandedSection === section ? "" : section;
        powerMenuOpen = false;
        selectedNetwork = null;
        wifiPassword = "";
        networkError = "";
        if (wifiDevice) wifiDevice.scannerEnabled = expandedSection === "network";
        if (bluetoothAdapter) bluetoothAdapter.discovering = expandedSection === "bluetooth" && bluetoothAdapter.enabled;
    }

    function close() {
        popupOpen = false;
        expandedSection = "";
        powerMenuOpen = false;
        selectedNetwork = null;
        if (wifiDevice) wifiDevice.scannerEnabled = false;
        if (bluetoothAdapter) bluetoothAdapter.discovering = false;
    }

    function togglePanel() {
        if (popupOpen) {
            close();
            return;
        }
        PopupManager.closeExcept("control-panel");
        popupOpen = true;
        refreshProfiles();
    }

    function connectNetwork(network) {
        networkError = "";
        if (network.connected) {
            network.device.disconnect();
        } else if (network.known) {
            network.connect();
        } else {
            selectedNetwork = network;
            wifiPassword = "";
        }
    }

    function submitNetworkPassword() {
        if (!selectedNetwork) return;
        if (wifiPassword.length < 8) {
            networkError = "The password must contain at least 8 characters.";
            return;
        }
        selectedNetwork.connectWithPsk(wifiPassword);
        selectedNetwork = null;
        wifiPassword = "";
    }

    function refreshProfiles() {
        if (!profileQuery.running) profileQuery.running = true;
    }

    function setProfile(profile) {
        if (!profilesAvailable || availableProfiles.indexOf(profile) < 0) return;
        profileSetter.command = ["powerprofilesctl", "set", profile];
        profileSetter.running = true;
    }

    function profileName(profile) {
        return profile === "power-saver" ? "Power Saver"
            : profile === "performance" ? "Performance" : "Balanced";
    }

    function profileDescription(profile) {
        return profile === "power-saver" ? "Reduce power use and extend battery life"
            : profile === "performance" ? "Favor speed and responsiveness"
            : "Balance performance and energy use";
    }

    function profileGlyph(profile) {
        return profile === "power-saver" ? "󰌪" : profile === "performance" ? "󰓅" : "󰾅";
    }

    function setSinkVolume(position, width) {
        if (!defaultSink || !defaultSink.audio || width <= 0) return;
        defaultSink.audio.volume = Math.max(0, Math.min(1.5, position / width * 1.5));
    }

    function runPowerAction(command) {
        close();
        Quickshell.execDetached(command);
    }

    width: statusIcons.implicitWidth + 20
    height: 28
    radius: 9
    color: panelMouse.containsMouse || popupOpen ? Theme.surfaceHover : Theme.surfaceRaised
    border.color: popupOpen ? Theme.accent : Theme.border
    border.width: 1

    RowLayout {
        id: statusIcons
        anchors.centerIn: parent
        spacing: 7

        AppText { text: control.networkIcon; color: Theme.text; font.pixelSize: 14 }
        AppText {
            visible: control.bluetoothAdapter !== null
            text: control.bluetoothIcon
            color: control.bluetoothAdapter && control.bluetoothAdapter.enabled ? Theme.text : Theme.textDim
            font.pixelSize: 14
        }
        AppText { text: control.volumeIcon; color: Theme.text; font.pixelSize: 14 }
        AppText {
            visible: control.profilesAvailable
            text: control.profileIcon
            color: Theme.text
            font.pixelSize: 14
        }
        AppText { text: "󰅀"; color: Theme.textDim; font.pixelSize: 11 }
    }

    MouseArea {
        id: panelMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: control.togglePanel()
        onWheel: wheel => {
            if (!control.defaultSink || !control.defaultSink.audio) return;
            const step = wheel.angleDelta.y > 0 ? 0.05 : -0.05;
            control.defaultSink.audio.volume = Math.max(0, Math.min(1.5,
                control.defaultSink.audio.volume + step));
            wheel.accepted = true;
        }
    }

    PwObjectTracker { objects: control.audioDevices }

    Process {
        id: profileQuery
        command: ["sh", "-c", "if ! command -v powerprofilesctl >/dev/null 2>&1; then printf 'missing\\n'; elif current=$(powerprofilesctl get 2>/dev/null); then printf 'current\\t%s\\n' \"$current\"; powerprofilesctl list 2>/dev/null | sed -n 's/^[[:space:]*]*\\([[:alnum:]-]*\\):.*/profile\\t\\1/p'; else printf 'missing\\n'; fi"]
        running: false
        stdout: StdioCollector {
            onStreamFinished: {
                const rows = this.text.trim().split("\n");
                const profiles = [];
                let active = "balanced";
                let available = rows.length > 0 && rows[0] !== "missing";
                for (const row of rows) {
                    const fields = row.split("\t");
                    if (fields[0] === "current" && fields[1]) active = fields[1];
                    if (fields[0] === "profile" && fields[1]) profiles.push(fields[1]);
                }
                control.profilesAvailable = available;
                control.currentProfile = active;
                control.availableProfiles = profiles;
            }
        }
    }

    Process {
        id: profileSetter
        running: false
        onExited: (exitCode, exitStatus) => {
            profileRefresh.restart();
        }
    }

    Timer {
        id: profileRefresh
        interval: 500
        repeat: false
        onTriggered: control.refreshProfiles()
    }

    Timer {
        interval: 5000
        running: true
        repeat: true
        onTriggered: control.refreshProfiles()
    }

    Component.onCompleted: refreshProfiles()

    component ToggleSwitch: Rectangle {
        id: toggleSwitch
        required property bool checked
        signal toggled()
        width: 42
        height: 24
        radius: 12
        color: checked ? Theme.accent : Theme.window
        border.color: checked ? Theme.accent : Theme.border
        border.width: 1

        Rectangle {
            x: toggleSwitch.checked ? parent.width - width - 4 : 4
            anchors.verticalCenter: parent.verticalCenter
            width: 16
            height: 16
            radius: 8
            color: toggleSwitch.checked ? Theme.accentText : Theme.textDim

            Behavior on x { NumberAnimation { duration: 120 } }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: toggleSwitch.toggled()
        }
    }

    component SectionHeader: Rectangle {
        id: sectionHeader
        required property string section
        required property string icon
        required property string title
        required property string subtitle
        property bool enabled: true
        property bool active: false
        property bool showToggle: false
        property bool toggleChecked: false
        signal toggleRequested()

        Layout.fillWidth: true
        implicitHeight: 68
        radius: 12
        color: headerMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised
        border.color: control.expandedSection === section ? Theme.accent : Theme.border
        border.width: 1
        opacity: enabled ? 1 : 0.55

        Rectangle {
            id: sectionIcon
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            width: 40
            height: 40
            radius: 20
            color: sectionHeader.active ? Theme.accent : Theme.window

            AppText {
                anchors.centerIn: parent
                text: sectionHeader.icon
                color: sectionHeader.active ? Theme.accentText : Theme.text
                font.pixelSize: 20
            }
        }

        Column {
            anchors.left: sectionIcon.right
            anchors.leftMargin: 10
            anchors.right: sectionToggle.visible ? sectionToggle.left : chevron.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            AppText {
                width: parent.width
                text: sectionHeader.title
                color: Theme.text
                font.pixelSize: 13
                font.bold: true
                elide: Text.ElideRight
            }
            AppText {
                width: parent.width
                text: sectionHeader.subtitle
                color: Theme.textDim
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }

        ToggleSwitch {
            id: sectionToggle
            visible: sectionHeader.showToggle
            checked: sectionHeader.toggleChecked
            anchors.right: chevron.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            enabled: sectionHeader.enabled
            onToggled: sectionHeader.toggleRequested()
            z: 2
        }

        AppText {
            id: chevron
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: control.expandedSection === sectionHeader.section ? "󰅃" : "󰅀"
            color: Theme.textDim
            font.pixelSize: 14
        }

        MouseArea {
            id: headerMouse
            anchors.fill: parent
            enabled: sectionHeader.enabled
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: control.setExpanded(sectionHeader.section)
        }
    }

    component DeviceRow: Rectangle {
        id: deviceRow
        required property string icon
        required property string title
        required property string subtitle
        property bool selected: false
        property bool busy: false
        property string actionText: ""
        signal activated()
        signal actionTriggered()

        Layout.fillWidth: true
        implicitHeight: 54
        radius: 10
        color: rowMouse.containsMouse ? Theme.surfaceHover : Theme.window
        border.color: selected ? Theme.accent : Theme.border
        border.width: 1

        AppText {
            id: rowIcon
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: deviceRow.icon
            color: deviceRow.selected ? Theme.accent : Theme.text
            font.pixelSize: 18
        }

        Column {
            anchors.left: rowIcon.right
            anchors.leftMargin: 11
            anchors.right: rowAction.visible ? rowAction.left : parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            AppText {
                width: parent.width
                text: deviceRow.title
                color: Theme.text
                font.pixelSize: 12
                font.bold: deviceRow.selected
                elide: Text.ElideRight
            }
            AppText {
                width: parent.width
                text: deviceRow.subtitle
                color: deviceRow.selected ? Theme.accent : Theme.textDim
                font.pixelSize: 10
                elide: Text.ElideRight
            }
        }

        Rectangle {
            id: rowAction
            visible: deviceRow.actionText.length > 0
            anchors.right: parent.right
            anchors.rightMargin: 9
            anchors.verticalCenter: parent.verticalCenter
            width: actionLabel.implicitWidth + 18
            height: 28
            radius: 8
            color: actionMouse.containsMouse ? Theme.accentStrong
                : deviceRow.selected ? Theme.accent : Theme.surfaceRaised

            AppText {
                id: actionLabel
                anchors.centerIn: parent
                text: deviceRow.busy ? "Working…" : deviceRow.actionText
                color: deviceRow.selected ? Theme.accentText : Theme.text
                font.pixelSize: 10
                font.bold: true
            }
            MouseArea {
                id: actionMouse
                anchors.fill: parent
                enabled: !deviceRow.busy
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: deviceRow.actionTriggered()
            }
        }

        MouseArea {
            id: rowMouse
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: rowAction.visible ? rowAction.left : parent.right
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: deviceRow.activated()
        }
    }

    PanelWindow {
        screen: control.panelScreen
        visible: control.popupOpen
        color: Theme.transparent
        anchors { top: true; bottom: true; left: true; right: true }

        WlrLayershell.namespace: "hyprland-control-panel"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

        Rectangle { anchors.fill: parent; color: Theme.overlay }
        MouseArea { anchors.fill: parent; onClicked: control.close() }

        Rectangle {
            id: card
            width: Math.min(410, parent.width - 24)
            height: Math.min(650, parent.height - 52)
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: 8
            anchors.rightMargin: 12
            radius: 14
            color: Theme.surface
            border.color: Theme.border
            border.width: 1
            focus: true
            Keys.onEscapePressed: control.close()

            MouseArea { anchors.fill: parent; onClicked: mouse.accepted = true }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 10

                Item {
                    Layout.fillWidth: true
                    implicitHeight: 36

                    Column {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 0
                        AppText { text: "Control Panel"; color: Theme.text; font.pixelSize: 18; font.bold: true }
                        AppText { text: "Quick settings"; color: Theme.textDim; font.pixelSize: 10 }
                    }

                    Rectangle {
                        id: powerButton
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 34
                        height: 34
                        radius: 17
                        color: powerMouse.containsMouse || control.powerMenuOpen ? Theme.accent : Theme.surfaceRaised
                        border.color: control.powerMenuOpen ? Theme.accent : Theme.border
                        border.width: 1
                        AppText {
                            anchors.centerIn: parent
                            text: "󰐥"
                            color: control.powerMenuOpen ? Theme.accentText : Theme.text
                            font.pixelSize: 17
                        }
                        MouseArea {
                            id: powerMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                control.powerMenuOpen = !control.powerMenuOpen;
                                control.expandedSection = "";
                            }
                        }
                    }
                }

                Rectangle {
                    visible: control.powerMenuOpen
                    Layout.fillWidth: true
                    implicitHeight: visible ? 72 : 0
                    radius: 12
                    color: Theme.window
                    border.color: Theme.border
                    border.width: 1

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 6
                        Repeater {
                            model: [
                                { icon: "󰍃", label: "Log Out", command: ["hyprctl", "dispatch", "exit"] },
                                { icon: "󰒲", label: "Suspend", command: ["systemctl", "suspend"] },
                                { icon: "󰜉", label: "Restart", command: ["systemctl", "reboot"] },
                                { icon: "󰐥", label: "Power Off", command: ["systemctl", "poweroff"] }
                            ]
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                radius: 9
                                color: actionMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised
                                Column {
                                    anchors.centerIn: parent
                                    spacing: 2
                                    AppText {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: modelData.icon
                                        color: modelData.label === "Power Off" ? Theme.accent : Theme.text
                                        font.pixelSize: 18
                                    }
                                    AppText {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: modelData.label
                                        color: Theme.textDim
                                        font.pixelSize: 9
                                    }
                                }
                                MouseArea {
                                    id: actionMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: control.runPowerAction(modelData.command)
                                }
                            }
                        }
                    }
                }

                Flickable {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentHeight: settingsColumn.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    ColumnLayout {
                        id: settingsColumn
                        width: parent.width
                        spacing: 8

                        SectionHeader {
                            section: "network"
                            icon: control.networkIcon
                            title: control.connectedNetworkDevice
                                && control.connectedNetworkDevice.type === DeviceType.Wired ? "Internet" : "Wi-Fi"
                            subtitle: control.networkSubtitle
                            active: control.connectedNetworkDevice !== null
                            enabled: Networking.backend === NetworkBackendType.NetworkManager
                            showToggle: control.wifiDevice !== null
                            toggleChecked: Networking.wifiEnabled
                            onToggleRequested: Networking.wifiEnabled = !Networking.wifiEnabled
                        }

                        ColumnLayout {
                            visible: control.expandedSection === "network"
                            Layout.fillWidth: true
                            spacing: 6

                            AppText {
                                Layout.fillWidth: true
                                text: !control.wifiDevice ? "No Wi-Fi adapter found"
                                    : Networking.wifiEnabled ? "Available networks" : "Wi-Fi is turned off"
                                color: Theme.textDim
                                font.pixelSize: 11
                            }

                            Repeater {
                                model: control.wifiDevice && Networking.wifiEnabled
                                    ? control.wifiDevice.networks.values : []
                                delegate: DeviceRow {
                                    required property var modelData
                                    icon: modelData.signalStrength < 0.25 ? "󰤟"
                                        : modelData.signalStrength < 0.5 ? "󰤢"
                                        : modelData.signalStrength < 0.75 ? "󰤥" : "󰤨"
                                    title: modelData.name
                                    subtitle: modelData.connected ? "Connected"
                                        : modelData.stateChanging ? "Connecting…"
                                        : modelData.known ? "Saved network" : "Secured network"
                                    selected: modelData.connected
                                    busy: modelData.stateChanging
                                    actionText: modelData.connected ? "Disconnect" : "Connect"
                                    onActionTriggered: control.connectNetwork(modelData)
                                    onActivated: control.connectNetwork(modelData)
                                }
                            }

                            Rectangle {
                                visible: control.selectedNetwork !== null
                                Layout.fillWidth: true
                                implicitHeight: 102
                                radius: 10
                                color: Theme.window
                                border.color: Theme.accent
                                border.width: 1

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 9
                                    spacing: 6
                                    AppText {
                                        Layout.fillWidth: true
                                        text: "Password for " + (control.selectedNetwork ? control.selectedNetwork.name : "network")
                                        color: Theme.text
                                        font.pixelSize: 11
                                        font.bold: true
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 6
                                        Rectangle {
                                            Layout.fillWidth: true
                                            implicitHeight: 32
                                            radius: 8
                                            color: Theme.surfaceRaised
                                            border.color: passwordField.activeFocus ? Theme.accent : Theme.border
                                            border.width: 1
                                            TextInput {
                                                id: passwordField
                                                anchors.fill: parent
                                                anchors.leftMargin: 10
                                                anchors.rightMargin: 10
                                                verticalAlignment: TextInput.AlignVCenter
                                                color: Theme.text
                                                font.family: Theme.fontFamily
                                                echoMode: TextInput.Password
                                                focus: control.selectedNetwork !== null
                                                onTextChanged: control.wifiPassword = text
                                                Keys.onReturnPressed: control.submitNetworkPassword()
                                            }
                                        }
                                        Rectangle {
                                            implicitWidth: 66
                                            implicitHeight: 32
                                            radius: 8
                                            color: connectMouse.containsMouse ? Theme.accentStrong : Theme.accent
                                            AppText { anchors.centerIn: parent; text: "Connect"; color: Theme.accentText; font.pixelSize: 10; font.bold: true }
                                            MouseArea {
                                                id: connectMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: control.submitNetworkPassword()
                                            }
                                        }
                                    }
                                    AppText {
                                        visible: control.networkError.length > 0
                                        Layout.fillWidth: true
                                        text: control.networkError
                                        color: Theme.accent
                                        font.pixelSize: 10
                                    }
                                }
                            }
                        }

                        SectionHeader {
                            section: "bluetooth"
                            icon: control.bluetoothIcon
                            title: "Bluetooth"
                            subtitle: control.bluetoothSubtitle
                            active: control.bluetoothAdapter && control.bluetoothAdapter.enabled
                            enabled: control.bluetoothAdapter !== null
                            showToggle: true
                            toggleChecked: control.bluetoothAdapter && control.bluetoothAdapter.enabled
                            onToggleRequested: {
                                if (control.bluetoothAdapter)
                                    control.bluetoothAdapter.enabled = !control.bluetoothAdapter.enabled;
                            }
                        }

                        ColumnLayout {
                            visible: control.expandedSection === "bluetooth"
                            Layout.fillWidth: true
                            spacing: 6

                            AppText {
                                Layout.fillWidth: true
                                text: !control.bluetoothAdapter ? "No Bluetooth adapter found"
                                    : control.bluetoothAdapter.enabled
                                        ? (control.bluetoothAdapter.discovering ? "Searching for devices…" : "Bluetooth devices")
                                        : "Bluetooth is turned off"
                                color: Theme.textDim
                                font.pixelSize: 11
                            }

                            Repeater {
                                model: control.bluetoothAdapter && control.bluetoothAdapter.enabled
                                    ? control.bluetoothDevices : []
                                delegate: DeviceRow {
                                    required property var modelData
                                    icon: modelData.icon && modelData.icon.indexOf("head") >= 0 ? "󰋋"
                                        : modelData.icon && modelData.icon.indexOf("input") >= 0 ? "󰌌" : "󰂯"
                                    title: modelData.name || modelData.deviceName || modelData.address
                                    subtitle: modelData.connected
                                        ? (modelData.batteryAvailable ? "Connected · " + Math.round(modelData.battery * 100) + "%" : "Connected")
                                        : modelData.pairing ? "Pairing…"
                                        : modelData.paired ? "Paired" : "Available"
                                    selected: modelData.connected
                                    busy: modelData.state === BluetoothDeviceState.Connecting
                                        || modelData.state === BluetoothDeviceState.Disconnecting || modelData.pairing
                                    actionText: modelData.connected ? "Disconnect"
                                        : modelData.paired ? "Connect" : "Pair"
                                    onActionTriggered: {
                                        if (modelData.connected) modelData.disconnect();
                                        else if (modelData.paired) modelData.connect();
                                        else modelData.pair();
                                    }
                                    onActivated: {
                                        if (modelData.connected) modelData.disconnect();
                                        else if (modelData.paired) modelData.connect();
                                        else modelData.pair();
                                    }
                                }
                            }
                        }

                        SectionHeader {
                            section: "power"
                            icon: control.profileIcon
                            title: "Power Mode"
                            subtitle: control.profilesAvailable ? control.profileLabel : "Unavailable"
                            active: control.profilesAvailable
                            enabled: control.profilesAvailable
                        }

                        ColumnLayout {
                            visible: control.expandedSection === "power"
                            Layout.fillWidth: true
                            spacing: 6

                            Repeater {
                                model: control.availableProfiles
                                delegate: DeviceRow {
                                    required property string modelData
                                    icon: control.profileGlyph(modelData)
                                    title: control.profileName(modelData)
                                    subtitle: control.profileDescription(modelData)
                                    selected: control.currentProfile === modelData
                                    actionText: selected ? "Active" : "Select"
                                    onActionTriggered: control.setProfile(modelData)
                                    onActivated: control.setProfile(modelData)
                                }
                            }
                        }

                        SectionHeader {
                            section: "sound"
                            icon: control.volumeIcon
                            title: "Sound"
                            subtitle: control.defaultSink
                                ? control.volumePercent + "% · " + (control.defaultSink.nickname || control.defaultSink.description || "Default output")
                                : "No output device"
                            active: control.defaultSink !== null && control.defaultSink.audio && !control.defaultSink.audio.muted
                            enabled: Pipewire.ready
                            showToggle: control.defaultSink !== null
                            toggleChecked: control.defaultSink !== null && control.defaultSink.audio && !control.defaultSink.audio.muted
                            onToggleRequested: {
                                if (control.defaultSink && control.defaultSink.audio)
                                    control.defaultSink.audio.muted = !control.defaultSink.audio.muted;
                            }
                        }

                        ColumnLayout {
                            visible: control.expandedSection === "sound"
                            Layout.fillWidth: true
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 9
                                AppText { text: control.volumeIcon; color: Theme.text; font.pixelSize: 17 }
                                Rectangle {
                                    id: volumeTrack
                                    Layout.fillWidth: true
                                    implicitHeight: 9
                                    radius: 5
                                    color: Theme.window
                                    border.color: Theme.border
                                    border.width: 1
                                    Rectangle {
                                        width: control.defaultSink && control.defaultSink.audio
                                            ? Math.min(parent.width, control.defaultSink.audio.volume / 1.5 * parent.width) : 0
                                        height: parent.height
                                        radius: parent.radius
                                        color: control.defaultSink && control.defaultSink.audio && control.defaultSink.audio.muted
                                            ? Theme.textDim : Theme.accent
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onPressed: control.setSinkVolume(mouse.x, width)
                                        onPositionChanged: if (pressed) control.setSinkVolume(mouse.x, width)
                                    }
                                }
                                AppText { text: control.volumePercent + "%"; color: Theme.textDim; font.pixelSize: 10 }
                            }

                            AppText { text: "Output device"; color: Theme.textDim; font.pixelSize: 11 }

                            Repeater {
                                model: control.outputDevices
                                delegate: DeviceRow {
                                    required property var modelData
                                    readonly property bool isDefault: control.defaultSink === modelData
                                    icon: "󰓃"
                                    title: modelData.nickname || modelData.description || modelData.name
                                    subtitle: isDefault ? "Default output" : Math.round(modelData.audio.volume * 100) + "%"
                                    selected: isDefault
                                    actionText: isDefault ? "Active" : "Select"
                                    onActionTriggered: Pipewire.preferredDefaultAudioSink = modelData
                                    onActivated: Pipewire.preferredDefaultAudioSink = modelData
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
