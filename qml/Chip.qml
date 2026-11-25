import QtQuick
import QtQuick.Controls

Rectangle {
    id: root
    property alias text: lbl.text
    property color tint: Theme.accent
    property string glyph
    implicitHeight: 24
    implicitWidth: row.implicitWidth + 16
    radius: 12
    color: Qt.rgba(tint.r, tint.g, tint.b, 0.16)
    Row {
        id: row
        anchors.centerIn: parent
        spacing: 5
        Label {
            visible: root.glyph !== ""
            text: root.glyph
            font.family: Theme.icons
            font.pixelSize: 11
            color: root.tint
            anchors.verticalCenter: parent.verticalCenter
        }
        Label {
            id: lbl
            font.pixelSize: Theme.fsSmall
            font.weight: Font.DemiBold
            color: root.tint
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
