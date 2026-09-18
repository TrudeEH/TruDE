import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import Quickshell.Wayland
import QtQuick
import QtQuick.Layouts
import "."

Item {
    id: profiles

    property bool popupOpen: false
    property int popupMonitorId: -1
    property bool available: false
    property string current: "balanced"
    readonly property var availableProfiles: [
        { name: "power-saver", label: "Power saver", subtitle: "Extend battery life", icon: "󰌪" },
        { name: "balanced", label: "Balanced", subtitle: "Everyday performance and efficiency", icon: "󰾅" },
        { name: "performance", label: "Performance", subtitle: "Prioritize speed and responsiveness", icon: "󰓅" }
    ]

    function close() {
        popupOpen = false;
        popupMonitorId = -1;
    }

    function closeOtherPopups() {
        PopupManager.closeExcept("power-profile");
    }

    function refresh() {
        currentProfile.running = true;
    }

    function setProfile(name) {
        close();
        Quickshell.execDetached(["powerprofilesctl", "set", name]);
        refreshTimer.restart();
    }

    Process {
        id: currentProfile
        command: ["sh", "-c", "if command -v powerprofilesctl >/dev/null 2>&1; then powerprofilesctl get; else printf 'missing\n'; fi"]
        running: false
        stdout: StdioCollector {
            onStreamFinished: {
                const value = this.text.trim();
                profiles.available = value !== "missing";
                if (profiles.available && value.length > 0) profiles.current = value;
            }
        }
    }

    Timer {
        id: refreshTimer
        interval: 800
        repeat: false
        onTriggered: profiles.refresh()
    }

    Timer {
        interval: 5000
        running: true
        repeat: true
        onTriggered: profiles.refresh()
    }

    Component.onCompleted: refresh()

    IpcHandler {
        target: "power-profile"

        function close(): void {
            profiles.close();
        }

        function toggle(): void {
            if (profiles.popupOpen) {
                profiles.close();
                return;
            }
            profiles.closeOtherPopups();
            if (!Hyprland.focusedMonitor) return;
            profiles.popupMonitorId = Hyprland.focusedMonitor.id;
            profiles.popupOpen = true;
            profiles.refresh();
        }
    }

    Variants {
        model: Quickshell.screens
        delegate: Component {
            PanelWindow {
                required property var modelData
                screen: modelData
                readonly property var monitor: Hyprland.monitorFor(screen)
                visible: profiles.popupOpen && monitor && monitor.id === profiles.popupMonitorId
                color: Theme.transparent
                anchors { top: true; bottom: true; left: true; right: true }

                WlrLayershell.namespace: "hyprland-power-profile"
                WlrLayershell.layer: WlrLayer.Overlay
                WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

                Rectangle {
                    anchors.fill: parent
                    color: Theme.overlay
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: profiles.close()
                }

                Rectangle {
                    id: card
                    width: Math.min(360, parent.width - 24)
                    height: 224
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.topMargin: 8
                    anchors.rightMargin: 14
                    radius: 12
                    color: Theme.surface
                    border.color: Theme.border
                    border.width: 1
                    focus: true
                    Keys.onEscapePressed: profiles.close()

                    MouseArea {
                        anchors.fill: parent
                        onClicked: mouse.accepted = true
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8

                        AppText {
                            Layout.fillWidth: true
                            text: "Power profile"
                            color: Theme.text
                            font.pixelSize: 16
                            font.bold: true
                        }

                        AppText {
                            Layout.fillWidth: true
                            text: profiles.available ? "Choose how the computer balances speed and battery life"
                                : "Install power-profiles-daemon to enable these controls"
                            color: Theme.textDim
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }

                        Repeater {
                            visible: profiles.available
                            model: profiles.availableProfiles
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: 48
                                radius: 8
                                color: modelData.name === profiles.current ? Theme.accent
                                    : profileMouse.containsMouse ? Theme.surfaceHover : Theme.surfaceRaised
                                border.color: modelData.name === profiles.current ? Theme.accent
                                    : profileMouse.containsMouse ? Theme.border : Theme.transparent
                                border.width: 1

                                AppText {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 13
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData.icon
                                    color: modelData.name === profiles.current ? Theme.accentText : Theme.text
                                    font.pixelSize: 19
                                }

                                ColumnLayout {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: 46
                                    anchors.rightMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 1
                                    AppText {
                                        Layout.fillWidth: true
                                        text: modelData.label + (modelData.name === profiles.current ? "  ·  Active" : "")
                                        color: modelData.name === profiles.current ? Theme.accentText : Theme.text
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                    AppText {
                                        Layout.fillWidth: true
                                        text: modelData.subtitle
                                        color: modelData.name === profiles.current ? Theme.accentText : Theme.textDim
                                        font.pixelSize: 10
                                    }
                                }

                                MouseArea {
                                    id: profileMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: profiles.setProfile(modelData.name)
                                }
                            }
                        }

                        AppText {
                            visible: !profiles.available
                            Layout.fillWidth: true
                            text: "Power profiles are unavailable"
                            color: Theme.textDim
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
            }
        }
    }
}
