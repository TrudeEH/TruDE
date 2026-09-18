import Quickshell
import Quickshell.Networking
import QtQuick
import "."

Rectangle {
    id: button
    property var devices: Networking.devices.values
    readonly property var connectedDevice: findConnectedDevice()
    readonly property var connectedNetwork: findConnectedNetwork(connectedDevice)
    readonly property string icon: {
        if (!connectedDevice) return "󰤭";
        return connectedDevice.type === DeviceType.Wifi ? "󰤨" : "󰈀";
    }
    readonly property string label: {
        if (!connectedDevice) return "Offline";
        if (connectedNetwork && connectedNetwork.name) return connectedNetwork.name;
        return connectedDevice.type === DeviceType.Wifi ? "Wi-Fi" : "Wired";
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

    width: Math.max(92, networkLabel.implicitWidth + 20)
    height: 28
    radius: 8
    color: networkMouse.containsMouse ? Theme.surfaceHover : Theme.transparent
    border.color: networkMouse.containsMouse ? Theme.border : Theme.transparent
    border.width: 1

    AppText {
        id: networkLabel
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        text: button.icon + "  " + button.label
        color: Theme.text
        font.pixelSize: 12
        elide: Text.ElideRight
    }

    MouseArea {
        id: networkMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: Quickshell.execDetached(["sh", "-c", "quickshell ipc call network toggle"])
    }
}
