import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import Quickshell.Services.Notifications
import Quickshell.Wayland
import QtQuick
import QtQuick.Layouts
import "."

Item {
    id: center

    property bool popupOpen: false
    property var popupScreen: null
    readonly property var notifications: notificationServer.trackedNotifications.values
    readonly property int count: notifications.length
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

    function toggle() {
        if (popupOpen) {
            close();
            return;
        }
        popupScreen = focusedScreen;
        popupOpen = popupScreen !== null;
    }

    NotificationServer {
        id: notificationServer
        keepOnReload: true
        persistenceSupported: true
        bodySupported: true
        bodyMarkupSupported: false
        bodyImagesSupported: true
        actionsSupported: true
        imageSupported: true

        onNotification: notification => {
            notification.tracked = true;
        }
    }

    IpcHandler {
        target: "notifications"

        function toggle(): void {
            center.toggle();
        }

        function close(): void {
            center.close();
        }

        function dismissAll(): void {
            for (const notification of center.notifications) notification.dismiss();
        }
    }

    Variants {
        model: Quickshell.screens
        delegate: Component {
            PanelWindow {
                required property var modelData
                screen: modelData
                readonly property var monitor: Hyprland.monitorFor(screen)
                visible: center.popupOpen && monitor
                    && monitor.id === Hyprland.monitorFor(center.popupScreen)?.id
                color: Theme.transparent
                anchors { top: true; bottom: true; left: true; right: true }

                WlrLayershell.namespace: "hyprland-notification-center"
                WlrLayershell.layer: WlrLayer.Overlay
                WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

                MouseArea {
                    anchors.fill: parent
                    onClicked: center.close()
                }

                Rectangle {
                    id: card
                    width: Math.min(parent.width - 28, 520)
                    height: Math.min(parent.height - 58, 600)
                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.topMargin: 44
                    radius: 12
                    color: Theme.surface
                    border.color: Theme.border
                    border.width: 1

                    MouseArea {
                        anchors.fill: parent
                        onClicked: mouse.accepted = true
                    }

                    ColumnLayout {
                        id: content
                        anchors.fill: parent
                        anchors.margins: 18
                        spacing: 10

                        RowLayout {
                            Layout.fillWidth: true

                            AppText {
                                Layout.fillWidth: true
                                text: "Notifications"
                                color: Theme.text
                                font.pixelSize: 18
                                font.bold: true
                            }

                            AppText {
                                text: center.count > 0 ? center.count + " active" : "All clear"
                                color: Theme.textDim
                                font.pixelSize: 12
                            }

                            AppText {
                                visible: center.count > 0
                                text: "󰎟"
                                color: clearMouse.containsMouse ? Theme.accentStrong : Theme.accent
                                font.pixelSize: 18

                                MouseArea {
                                    id: clearMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: center.dismissAll()
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: Theme.border
                        }

                        AppText {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: center.count === 0
                            text: "No notifications"
                            color: Theme.textDim
                            font.pixelSize: 13
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        ListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: center.count > 0
                            clip: true
                            spacing: 8
                            model: notificationServer.trackedNotifications

                            delegate: Rectangle {
                                required property var modelData
                                width: ListView.view.width
                                implicitHeight: notificationContent.implicitHeight + 24
                                radius: 9
                                color: Theme.surfaceRaised
                                border.color: Theme.border
                                border.width: 1

                                RowLayout {
                                    id: notificationContent
                                    anchors.fill: parent
                                    anchors.margins: 12
                                    spacing: 10

                                    Image {
                                        Layout.alignment: Qt.AlignTop
                                        Layout.preferredWidth: 34
                                        Layout.preferredHeight: 34
                                        visible: modelData.image !== "" || modelData.appIcon !== ""
                                        source: modelData.image !== "" ? modelData.image
                                            : Quickshell.iconPath(modelData.appIcon, true)
                                        fillMode: Image.PreserveAspectFit
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 3

                                        AppText {
                                            Layout.fillWidth: true
                                            text: modelData.appName !== "" ? modelData.appName : "Notification"
                                            color: Theme.textDim
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                        }

                                        AppText {
                                            Layout.fillWidth: true
                                            text: modelData.summary
                                            color: Theme.text
                                            font.pixelSize: 13
                                            font.bold: true
                                            wrapMode: Text.Wrap
                                        }

                                        AppText {
                                            Layout.fillWidth: true
                                            visible: modelData.body !== ""
                                            text: modelData.body
                                            color: Theme.textSecondary
                                            font.pixelSize: 12
                                            wrapMode: Text.Wrap
                                            maximumLineCount: 4
                                            elide: Text.ElideRight
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            visible: modelData.actions.length > 0
                                            spacing: 6

                                            Repeater {
                                                model: modelData.actions
                                                delegate: Rectangle {
                                                    required property var modelData
                                                    implicitWidth: actionLabel.implicitWidth + 16
                                                    implicitHeight: 26
                                                    radius: 6
                                                    color: actionMouse.containsMouse ? Theme.accentStrong : Theme.accent

                                                    AppText {
                                                        id: actionLabel
                                                        anchors.centerIn: parent
                                                        text: modelData.text
                                                        color: Theme.accentText
                                                        font.pixelSize: 11
                                                    }

                                                    MouseArea {
                                                        id: actionMouse
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: modelData.invoke()
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    AppText {
                                        Layout.alignment: Qt.AlignTop
                                        text: "󰅖"
                                        color: dismissMouse.containsMouse ? Theme.accentStrong : Theme.textDim
                                        font.pixelSize: 16

                                        MouseArea {
                                            id: dismissMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: modelData.dismiss()
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
