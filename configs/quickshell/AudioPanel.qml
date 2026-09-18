import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import Quickshell.Services.Pipewire
import Quickshell.Wayland
import QtQuick
import QtQuick.Layouts
import "."

Item {
    id: audio
    property bool popupOpen: false
    property var popupScreen: null
    property bool showingOutputs: true
    readonly property var nodes: Pipewire.nodes.values
    readonly property var audioDevices: nodes.filter(node => node.audio && !node.isStream)
    readonly property var outputDevices: audioDevices.filter(node => node.ready && node.isSink)
    readonly property var inputDevices: audioDevices.filter(node => node.ready && !node.isSink)
    readonly property var focusedScreen: {
        const focused = Hyprland.focusedMonitor;
        if (!focused) return null;
        for (const screen of Quickshell.screens) {
            if (screen.name === focused.name) return screen;
        }
        return null;
    }

    function close() {
        popupOpen = false;
        popupScreen = null;
    }

    function closeOtherPopups() {
        for (const target of ["network", "audio", "power", "notifications", "launcher"]) {
            if (target !== "audio")
                Quickshell.execDetached(["quickshell", "ipc", "call", target, "close"]);
        }
    }

    function toggle() {
        if (popupOpen) {
            close();
            return;
        }
        audio.closeOtherPopups();
        popupScreen = focusedScreen;
        popupOpen = popupScreen !== null;
    }

    function selectDevice(node) {
        if (showingOutputs) Pipewire.preferredDefaultAudioSink = node;
        else Pipewire.preferredDefaultAudioSource = node;
    }

    function setVolume(node, position, width) {
        if (!node || !node.audio || width <= 0) return;
        node.audio.volume = Math.max(0, Math.min(1.5, position / width * 1.5));
    }

    PwObjectTracker {
        objects: audio.audioDevices
    }

    IpcHandler {
        target: "audio"
        function toggle(): void {
            audio.toggle();
        }

        function close(): void {
            audio.close();
        }
    }

    PanelWindow {
        screen: audio.popupScreen
        visible: audio.popupOpen && audio.popupScreen !== null
        color: Theme.transparent
        anchors { top: true; bottom: true; left: true; right: true }

        WlrLayershell.namespace: "hyprland-audio"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

        MouseArea {
            anchors.fill: parent
            onClicked: audio.close()
        }

        Rectangle {
            id: card
            width: 420
            height: 520
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: 8
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
                anchors.margins: 16
                spacing: 10

                Item {
                    Layout.fillWidth: true
                    implicitHeight: 30

                    AppText {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Audio"
                        color: Theme.text
                        font.pixelSize: 20
                        font.bold: true
                    }

                    AppText {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: Pipewire.ready ? "PipeWire" : "Unavailable"
                        color: Theme.textDim
                        font.pixelSize: 11
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Repeater {
                        model: [
                            { label: "Output", icon: "󰕾", output: true },
                            { label: "Input", icon: "󰍬", output: false }
                        ]
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: 36
                            radius: 8
                            color: audio.showingOutputs === modelData.output
                                ? Theme.accent : (tabMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised)
                            border.color: tabMouse.containsMouse ? Theme.border : Theme.transparent
                            border.width: 1

                            AppText {
                                anchors.centerIn: parent
                                text: modelData.icon + "  " + modelData.label
                                color: audio.showingOutputs === modelData.output ? Theme.accentText : Theme.text
                                font.pixelSize: 12
                                font.bold: audio.showingOutputs === modelData.output
                            }
                            MouseArea {
                                id: tabMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: audio.showingOutputs = modelData.output
                            }
                        }
                    }
                }

                Flickable {
                    id: deviceList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentHeight: deviceColumn.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    ColumnLayout {
                        id: deviceColumn
                        width: deviceList.width
                        spacing: 8

                        AppText {
                            Layout.fillWidth: true
                            text: audio.showingOutputs ? "Output devices" : "Input devices"
                            color: Theme.textDim
                            font.pixelSize: 11
                            font.bold: true
                        }

                        Repeater {
                            model: audio.showingOutputs ? audio.outputDevices : audio.inputDevices
                            delegate: Rectangle {
                                required property var modelData
                                readonly property bool selected: audio.showingOutputs
                                    ? Pipewire.defaultAudioSink === modelData
                                    : Pipewire.defaultAudioSource === modelData
                                Layout.fillWidth: true
                                implicitHeight: 106
                                radius: 10
                                color: selected ? Theme.surfaceHover : Theme.surfaceRaised
                                border.color: selected ? Theme.accent : Theme.border
                                border.width: 1

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 12
                                    spacing: 7

                                    Item {
                                        Layout.fillWidth: true
                                        implicitHeight: 24

                                        AppText {
                                            anchors.left: parent.left
                                            anchors.right: muteButton.left
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.rightMargin: 8
                                            text: modelData.nickname || modelData.description || modelData.name
                                            color: Theme.text
                                            font.pixelSize: 12
                                            font.bold: selected
                                            elide: Text.ElideRight
                                        }

                                        AppText {
                                            anchors.right: muteButton.left
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.rightMargin: 28
                                            text: Math.round(modelData.audio.volume * 100) + "%"
                                            color: Theme.textDim
                                            font.pixelSize: 11
                                        }

                                        Rectangle {
                                            id: muteButton
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 24
                                            height: 24
                                            radius: 6
                                            color: muteMouse.containsMouse ? Theme.surfaceHover : Theme.transparent

                                            AppText {
                                                anchors.centerIn: parent
                                                text: modelData.audio.muted ? "󰖁" : "󰕾"
                                                color: modelData.audio.muted ? Theme.accent : Theme.text
                                                font.pixelSize: 16
                                            }
                                            MouseArea {
                                                id: muteMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: modelData.audio.muted = !modelData.audio.muted
                                            }
                                        }
                                    }

                                    Rectangle {
                                        id: volumeTrack
                                        Layout.fillWidth: true
                                        implicitHeight: 8
                                        radius: 4
                                        color: Theme.window

                                        Rectangle {
                                            width: Math.min(parent.width, Math.max(0, modelData.audio.volume / 1.5 * parent.width))
                                            height: parent.height
                                            radius: parent.radius
                                            color: modelData.audio.muted ? Theme.textDim : Theme.accent
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onPressed: audio.setVolume(modelData, mouse.x, width)
                                            onPositionChanged: if (pressed) audio.setVolume(modelData, mouse.x, width)
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        z: -1
                                        onClicked: audio.selectDevice(modelData)
                                    }
                                }
                            }
                        }

                        AppText {
                            Layout.fillWidth: true
                            visible: (audio.showingOutputs ? audio.outputDevices : audio.inputDevices).length === 0
                            text: "No devices available"
                            color: Theme.textDim
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }

                AppText {
                    Layout.fillWidth: true
                    text: "Click a device to make it the default"
                    color: Theme.textDim
                    font.pixelSize: 11
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }
    }
}
