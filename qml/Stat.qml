import QtQuick
import QtQuick.Controls

// key figure: small caption over a large value
Column {
    id: root
    property string caption
    property string value
    property string unit
    property color valueColor: Theme.text
    spacing: 1
    Label {
        text: root.caption
        color: Theme.text3
        font.pixelSize: 11
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.4
    }
    Row {
        spacing: 4
        Label {
            id: v
            text: root.value
            color: root.valueColor
            font.family: Theme.display
            font.pixelSize: 20
            font.weight: Font.DemiBold
        }
        Label {
            text: root.unit
            color: Theme.text2
            font.pixelSize: Theme.fsSmall
            anchors.baseline: v.baseline
        }
    }
}
