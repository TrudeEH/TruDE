import Quickshell
import Quickshell.Widgets
import QtQuick
import QtQuick.Layouts
import "."

Item {
    id: root

    property var menu
    property var activateEntry
    property bool hovered: menuMouse.containsMouse || childHover
    readonly property bool childHover: submenuLoader.item ? submenuLoader.item.hovered : false

    implicitWidth: 272
    implicitHeight: menuColumn.implicitHeight

    QsMenuOpener {
        id: opener
        menu: root.menu
    }

    Column {
        id: menuColumn
        width: root.implicitWidth
        spacing: 2

        Repeater {
            model: opener.children

            delegate: Item {
                id: menuEntry
                required property var modelData
                width: menuColumn.width
                height: modelData.isSeparator ? 9 : 34

                Rectangle {
                    visible: menuEntry.modelData.isSeparator
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    height: 1
                    color: Theme.border
                }

                Rectangle {
                    visible: !menuEntry.modelData.isSeparator
                    anchors.fill: parent
                    radius: 7
                    color: menuEntry.modelData.enabled && entryMouse.containsMouse
                        ? Theme.surfaceHover
                        : Theme.transparent
                }

                IconImage {
                    visible: !menuEntry.modelData.isSeparator && menuEntry.modelData.icon !== ""
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18
                    height: 18
                    source: menuEntry.modelData.icon
                    mipmap: true
                    opacity: menuEntry.modelData.enabled ? 1 : 0.45
                }

                AppText {
                    visible: !menuEntry.modelData.isSeparator
                    anchors.left: parent.left
                    anchors.leftMargin: menuEntry.modelData.icon !== "" ? 36 : 12
                    anchors.right: submenuArrow.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: menuEntry.modelData.text
                    color: menuEntry.modelData.enabled ? Theme.text : Theme.textDim
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }

                AppText {
                    id: checkMark
                    visible: !menuEntry.modelData.isSeparator
                        && menuEntry.modelData.buttonType !== QsMenuButtonType.None
                        && menuEntry.modelData.checkState !== Qt.Unchecked
                    anchors.right: submenuArrow.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: menuEntry.modelData.buttonType === QsMenuButtonType.RadioButton ? "●" : "✓"
                    color: Theme.accent
                    font.pixelSize: 13
                }

                AppText {
                    id: submenuArrow
                    visible: !menuEntry.modelData.isSeparator && menuEntry.modelData.hasChildren
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: "›"
                    color: Theme.textDim
                    font.pixelSize: 18
                }

                MouseArea {
                    id: entryMouse
                    visible: !menuEntry.modelData.isSeparator
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: menuEntry.modelData.enabled
                    onEntered: {
                        if (menuEntry.modelData.hasChildren) {
                            childHover = false;
                        }
                    }
                    onClicked: {
                        if (menuEntry.modelData.hasChildren) {
                            return;
                        }
                        root.activateEntry(menuEntry.modelData);
                    }
                }

                Loader {
                    id: submenuLoader
                    active: menuEntry.modelData.hasChildren
                        && (entryMouse.containsMouse || (item && item.hovered))
                    x: root.width - 4
                    y: 0
                    z: 10
                    source: "TrayMenuView.qml"
                    onLoaded: {
                        item.menu = menuEntry.modelData;
                        item.activateEntry = root.activateEntry;
                    }
                }
            }
        }
    }

    MouseArea {
        id: menuMouse
        anchors.fill: parent
        enabled: false
    }
}
