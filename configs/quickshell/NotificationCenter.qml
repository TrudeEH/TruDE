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
    property var toastNotification: null
    property var toastScreen: null
    property bool toastOpen: false
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

    function closeToast() {
        toastOpen = false;
        toastNotification = null;
        toastScreen = null;
    }

    function activateToast() {
        if (toastNotification) center.activate(toastNotification);
        else closeToast();
    }

    function toggle() {
        if (popupOpen) {
            close();
            return;
        }
        popupScreen = focusedScreen;
        popupOpen = popupScreen !== null;
    }

    Timer {
        id: toastTimer
        interval: 5000
        repeat: false
        onTriggered: center.closeToast()
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
            center.toastNotification = notification;
            center.toastScreen = center.focusedScreen;
            center.toastOpen = center.toastScreen !== null;
            toastTimer.restart();
        }
    }

    function activate(notification) {
        for (const action of notification.actions) {
            if (action.identifier === "default") {
                action.invoke();
                center.close();
                return;
            }
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
            const activeNotifications = [];
            const trackedNotifications = notificationServer.trackedNotifications.values;
            for (let index = 0; index < trackedNotifications.length; index++)
                activeNotifications.push(trackedNotifications[index]);
            center.close();
            for (const notification of activeNotifications) {
                notification.dismiss();
                notification.tracked = false;
            }
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
                    z: 1
                    width: Math.min(parent.width - 28, 520)
                    height: Math.min(parent.height - 58, 600)
                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.topMargin: 8
                    radius: 12
                    color: Theme.surface
                    border.color: Theme.border
                    border.width: 1
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

                                MouseArea {
                                    anchors.fill: parent
                                    z: 0
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: center.activate(modelData)
                                }

                                RowLayout {
                                    id: notificationContent
                                    anchors.fill: parent
                                    z: 1
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

                                    }

                                    AppText {
                                        Layout.alignment: Qt.AlignTop
                                        text: "󰅖"
                                        color: dismissMouse.containsMouse ? Theme.accentStrong : Theme.textDim
                                        z: 2
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
                Rectangle {
                    id: clearButton
                    visible: center.count > 0
                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.horizontalCenterOffset: Math.min(parent.width - 28, 520) / 2 - 14 - width / 2
                    anchors.topMargin: 22
                    width: 78
                    height: 26
                    radius: 6
                    color: clearMouse.containsMouse ? Theme.accentStrong : Theme.accent
                    z: 10

                    AppText {
                        anchors.centerIn: parent
                        text: "Clear all"
                        color: Theme.accentText
                        font.pixelSize: 11
                    }

                    MouseArea {
                        id: clearMouse
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            mouse.accepted = true;
                            Quickshell.execDetached(["quickshell", "ipc", "call", "notifications", "dismissAll"]);
                        }
                    }
                }
            }
        }
    }

    Variants {
        model: Quickshell.screens
        delegate: Component {
            PanelWindow {
                required property var modelData
                screen: modelData
                visible: center.toastOpen && center.toastScreen
                    && modelData.name === center.toastScreen.name
                color: Theme.transparent
                anchors { top: true; left: true; right: true }
                implicitHeight: 132

                WlrLayershell.namespace: "hyprland-notification-toast"
                WlrLayershell.layer: WlrLayer.Overlay
                WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

                Rectangle {
                    id: toast
                    width: Math.min(parent.width - 28, 520)
                    height: Math.min(parent.height - 16, 104)
                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.topMargin: 8
                    radius: 12
                    color: Theme.surface
                    border.color: Theme.border
                    border.width: 1

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: center.activateToast()
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 10

                        Image {
                            Layout.alignment: Qt.AlignTop
                            Layout.preferredWidth: 38
                            Layout.preferredHeight: 38
                            visible: center.toastNotification
                                && (center.toastNotification.image !== ""
                                    || center.toastNotification.appIcon !== "")
                            source: center.toastNotification
                                && center.toastNotification.image !== ""
                                ? center.toastNotification.image
                                : Quickshell.iconPath(center.toastNotification
                                    ? center.toastNotification.appIcon : "", true)
                            fillMode: Image.PreserveAspectFit
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3

                            AppText {
                                Layout.fillWidth: true
                                text: center.toastNotification
                                    ? (center.toastNotification.appName !== ""
                                        ? center.toastNotification.appName : "Notification")
                                    : "Notification"
                                color: Theme.textDim
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }

                            AppText {
                                Layout.fillWidth: true
                                text: center.toastNotification
                                    ? center.toastNotification.summary : ""
                                color: Theme.text
                                font.pixelSize: 13
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            AppText {
                                Layout.fillWidth: true
                                visible: center.toastNotification
                                    && center.toastNotification.body !== ""
                                text: center.toastNotification
                                    ? center.toastNotification.body : ""
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                        }

                        AppText {
                            Layout.alignment: Qt.AlignTop
                            text: "󰅖"
                            color: toastDismissMouse.containsMouse
                                ? Theme.accentStrong : Theme.textDim
                            font.pixelSize: 16

                            MouseArea {
                                id: toastDismissMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    mouse.accepted = true;
                                    if (center.toastNotification)
                                        center.toastNotification.dismiss();
                                    center.closeToast();
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
