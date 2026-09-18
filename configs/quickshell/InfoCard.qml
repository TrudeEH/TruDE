import QtQuick
import QtQuick.Layouts
import "."

Rectangle {
    id: card

    property string icon: ""
    property string title: ""
    property string value: "—"
    property string detail: ""
    property bool highlighted: false

    implicitHeight: 86
    radius: 10
    color: highlighted ? Theme.surfaceHover : Theme.surfaceRaised
    border.color: highlighted ? Theme.accent : Theme.border
    border.width: 1

    RowLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        Rectangle {
            Layout.alignment: Qt.AlignTop
            implicitWidth: 30
            implicitHeight: 30
            radius: 9
            color: card.highlighted ? Theme.accent : Theme.surface

            AppText {
                anchors.centerIn: parent
                text: card.icon
                color: card.highlighted ? Theme.accentText : Theme.accent
                font.pixelSize: 16
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: 2

            AppText {
                text: card.title.toUpperCase()
                color: Theme.textDim
                font.pixelSize: 10
                font.bold: true
            }

            AppText {
                Layout.fillWidth: true
                text: card.value
                color: Theme.text
                font.pixelSize: 14
                font.bold: true
                elide: Text.ElideRight
                maximumLineCount: 2
                wrapMode: Text.Wrap
            }

            AppText {
                visible: card.detail.length > 0
                Layout.fillWidth: true
                text: card.detail
                color: Theme.textDim
                font.pixelSize: 10
                elide: Text.ElideRight
                maximumLineCount: 1
            }
        }
    }
}
