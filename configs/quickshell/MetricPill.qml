import QtQuick
import QtQuick.Layouts
import "."

Rectangle {
    id: pill

    property string icon: ""
    property string label: ""
    property string value: ""
    property bool highlighted: false

    implicitWidth: content.implicitWidth + 18
    implicitHeight: 24
    radius: 7
    color: highlighted ? Theme.accent : Theme.surface
    border.color: highlighted ? Theme.accent : Theme.border
    border.width: 1

    RowLayout {
        id: content
        anchors.centerIn: parent
        spacing: 5

        AppText {
            text: pill.icon
            color: highlighted ? Theme.accentText : Theme.textDim
            font.pixelSize: 13
        }

        AppText {
            visible: pill.label.length > 0
            text: pill.label
            color: highlighted ? Theme.accentText : Theme.textDim
            font.pixelSize: 10
        }

        AppText {
            text: pill.value
            color: highlighted ? Theme.accentText : Theme.text
            font.pixelSize: 11
            font.bold: true
        }
    }
}
