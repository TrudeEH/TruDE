import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import Quickshell.Networking
import Quickshell.Wayland
import QtQuick
import QtQuick.Layouts
import "."

Item {
    id: network
    property bool popupOpen: false
    property var popupScreen: null
    property var devices: Networking.devices.values
    property var selectedNetwork: null
    property string passwordText: ""
    property string errorText: ""
    readonly property bool available: Networking.backend === NetworkBackendType.NetworkManager
    readonly property var wifiDevice: findWifiDevice()
    readonly property var connectedDevice: findConnectedDevice()
    readonly property var connectedNetwork: findConnectedNetwork(connectedDevice)
    readonly property var focusedScreen: {
        const focused = Hyprland.focusedMonitor;
        if (!focused) return null;
        for (const screen of Quickshell.screens) {
            if (screen.name === focused.name) return screen;
        }
        return null;
    }
    readonly property string statusText: {
        if (!connectedDevice) return "Not connected";
        if (connectedNetwork && connectedNetwork.name) return connectedNetwork.name;
        return connectedDevice.type === DeviceType.Wifi ? "Wi-Fi connected" : "Wired connected";
    }

    function findWifiDevice() {
        for (const device of devices) {
            if (device.type === DeviceType.Wifi) return device;
        }
        return null;
    }

    function findConnectedDevice() {
        for (const device of devices) {
            if (device.connected) return device;
        }
        return null;
    }

    function findConnectedNetwork(device) {
        if (!device) return null;
        if (device.type === DeviceType.Wifi) {
            for (const connection of device.networks.values) {
                if (connection.connected) return connection;
            }
        } else if (device.type === DeviceType.Wired && device.network) {
            return device.network;
        }
        return null;
    }

    function scan() {
        if (popupOpen && wifiDevice) wifiDevice.scannerEnabled = true;
    }

    function stopScan() {
        if (wifiDevice) wifiDevice.scannerEnabled = false;
    }

    function closePopup() {
        popupOpen = false;
        popupScreen = null;
        stopScan();
    }

    function connectSelected() {
        if (!selectedNetwork || !passwordText) return;
        errorText = "";
        selectedNetwork.connectWithPsk(passwordText);
        passwordText = "";
    }

    IpcHandler {
        target: "network"
        function toggle(): void {
            if (network.popupOpen) {
                network.closePopup();
                return;
            }
            network.popupScreen = network.focusedScreen;
            network.popupOpen = network.popupScreen !== null;
            if (network.popupOpen) network.scan();
        }
    }

    Connections {
        target: Networking.devices
        function onValuesChanged() {
            if (network.popupOpen) network.scan();
        }
    }

    Connections {
        target: network.selectedNetwork
        function onConnectionFailed(reason) {
            network.errorText = ConnectionFailReason.toString(reason);
        }
        function onConnectedChanged() {
            if (target && target.connected) network.selectedNetwork = null;
        }
    }

    PanelWindow {
                screen: network.popupScreen
                visible: network.popupOpen && network.popupScreen !== null
                color: Theme.transparent
                anchors { top: true; bottom: true; left: true; right: true }

                WlrLayershell.namespace: "hyprland-network"
                WlrLayershell.layer: WlrLayer.Overlay
                WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

                MouseArea {
                    anchors.fill: parent
                    onClicked: network.closePopup()
                }

                Rectangle {
                    id: card
                    width: 390
                    height: 560
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.topMargin: 44
                    anchors.rightMargin: 14
                    radius: 12
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
                        spacing: 10

                        Text {
                            Layout.fillWidth: true
                            text: network.statusText
                            color: Theme.text
                            font.pixelSize: 14
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 46
                            radius: 8
                            color: Theme.surfaceRaised
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 8
                                Text {
                                    Layout.fillWidth: true
                                    text: network.available
                                        ? "Connection · " + NetworkConnectivity.toString(Networking.connectivity)
                                        : "NetworkManager unavailable"
                                    color: Theme.textSecondary
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }
                                Rectangle {
                                    implicitWidth: 68
                                    implicitHeight: 28
                                    radius: 14
                                    visible: network.connectedDevice !== null
                                    color: disconnectMouse.containsMouse ? Theme.surfaceHover : Theme.transparent
                                    border.color: Theme.border
                                    border.width: 1
                                    Text {
                                        anchors.centerIn: parent
                                        text: "Disconnect"
                                        color: Theme.textDim
                                        font.pixelSize: 11
                                    }
                                    MouseArea {
                                        id: disconnectMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: network.connectedDevice.disconnect()
                                    }
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: network.wifiDevice !== null
                            Text {
                                Layout.fillWidth: true
                                text: network.wifiDevice ? "Wi-Fi  ·  " + network.wifiDevice.name : ""
                                color: Theme.text
                                font.bold: true
                            }
                            Rectangle {
                                implicitWidth: 58
                                implicitHeight: 28
                                radius: 14
                                color: Networking.wifiEnabled ? Theme.accent : Theme.surfaceRaised
                                Text {
                                    anchors.centerIn: parent
                                    text: Networking.wifiEnabled ? "On" : "Off"
                                    color: Networking.wifiEnabled ? Theme.accentText : Theme.textDim
                                    font.pixelSize: 11
                                    font.bold: true
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Networking.wifiEnabled = !Networking.wifiEnabled
                                }
                            }
                            Rectangle {
                                implicitWidth: 52
                                implicitHeight: 28
                                radius: 14
                                color: scanMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised
                                Text {
                                    anchors.centerIn: parent
                                    text: "Scan"
                                    color: Theme.text
                                    font.pixelSize: 11
                                }
                                MouseArea {
                                    id: scanMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: network.scan()
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: network.wifiDevice !== null
                            text: "Available networks"
                            color: Theme.textDim
                            font.pixelSize: 12
                        }

                        ListView {
                            id: networkList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 2
                            model: network.wifiDevice ? network.wifiDevice.networks : null
                            delegate: Rectangle {
                                required property var modelData
                                width: networkList.width
                                height: 44
                                radius: 7
                                color: networkMouse.containsMouse || modelData.connected
                                    ? Theme.surfaceHover : Theme.transparent

                                ColumnLayout {
                                    anchors.left: parent.left
                                    anchors.right: signalText.left
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 1
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        color: Theme.text
                                        font.pixelSize: 12
                                        font.bold: modelData.connected
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.known ? "Saved network" : WifiSecurityType.toString(modelData.security)
                                        color: Theme.textDim
                                        font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }
                                }
                                Text {
                                    id: signalText
                                    anchors.right: parent.right
                                    anchors.rightMargin: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: Math.round(modelData.signalStrength * 100) + "%"
                                    color: modelData.connected ? Theme.accent : Theme.textDim
                                    font.pixelSize: 11
                                }
                                MouseArea {
                                    id: networkMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        network.errorText = "";
                                        network.selectedNetwork = modelData;
                                        if (modelData.known || modelData.security === WifiSecurityType.Open) {
                                            modelData.connect();
                                            network.selectedNetwork = null;
                                        } else {
                                            network.passwordText = "";
                                        }
                                    }
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: network.wifiDevice === null
                            text: network.available ? "No Wi-Fi adapter found" : "Install NetworkManager to manage networks"
                            color: Theme.textDim
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: network.selectedNetwork !== null
                            spacing: 8
                            Text {
                                text: "Connect to " + (network.selectedNetwork ? network.selectedNetwork.name : "")
                                color: Theme.text
                                font.bold: true
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 40
                                radius: 8
                                color: Theme.surfaceRaised
                                border.color: passwordField.activeFocus ? Theme.accent : Theme.border
                                border.width: 1
                                TextInput {
                                    id: passwordField
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    verticalAlignment: TextInput.AlignVCenter
                                    color: Theme.text
                                    echoMode: TextInput.Password
                                    focus: network.selectedNetwork !== null
                                    onTextChanged: network.passwordText = text
                                    Keys.onReturnPressed: network.connectSelected()
                                }
                                Text {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: passwordField.text.length === 0
                                    text: "Wi-Fi password"
                                    color: Theme.textDim
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Item { Layout.fillWidth: true }
                                Rectangle {
                                    implicitWidth: 68
                                    implicitHeight: 30
                                    radius: 15
                                    color: connectMouse.containsMouse ? Theme.accentStrong : Theme.accent
                                    Text {
                                        anchors.centerIn: parent
                                        text: "Connect"
                                        color: Theme.accentText
                                        font.pixelSize: 11
                                        font.bold: true
                                    }
                                    MouseArea {
                                        id: connectMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: network.connectSelected()
                                    }
                                }
                                Rectangle {
                                    implicitWidth: 58
                                    implicitHeight: 30
                                    radius: 15
                                    visible: network.selectedNetwork && network.selectedNetwork.known
                                    color: forgetMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised
                                    Text {
                                        anchors.centerIn: parent
                                        text: "Forget"
                                        color: Theme.textDim
                                        font.pixelSize: 11
                                    }
                                    MouseArea {
                                        id: forgetMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            network.selectedNetwork.forget();
                                            network.selectedNetwork = null;
                                        }
                                    }
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: network.errorText.length > 0
                                text: network.errorText
                                color: Theme.accent
                                font.pixelSize: 11
                                wrapMode: Text.WordWrap
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: network.connectedDevice !== null
                            text: network.connectedDevice
                                ? network.connectedDevice.name + "  ·  " + network.connectedDevice.address
                                : ""
                            color: Theme.textDim
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                    }
                    focus: true
                    Keys.onEscapePressed: network.closePopup()
                }
    }
}
