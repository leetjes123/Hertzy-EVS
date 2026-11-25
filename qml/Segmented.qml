import QtQuick
import QtQuick.Controls

// compact segmented control: flat button group with a lit segment (EVS override of qt/qml/Segmented.qml)
Rectangle {
    id: root
    property var model: []
    property int currentIndex: 0
    property int itemPadding: 10
    property int fontSize: Theme.fs
    property bool fill: false          // stretch segments over the full width
    signal activated(int index)
    implicitHeight: 26
    implicitWidth: row.implicitWidth + 2
    radius: Theme.radius
    color: Theme.field
    border.color: Theme.border
    Row {
        id: meas
        visible: false
        Repeater { model: root.model; Label { required property var modelData; text: modelData; font.pixelSize: root.fontSize; font.weight: Font.DemiBold } }
    }
    readonly property real extra: Math.max(8, (width - 2 - Math.max(0, rep.count - 1) - meas.implicitWidth) / Math.max(1, rep.count))
    Row {
        id: row
        anchors.centerIn: parent
        spacing: 1
        Repeater {
            id: rep
            model: root.model
            delegate: Rectangle {
                required property int index
                required property var modelData
                height: root.height - 2
                width: root.fill ? lblBold.implicitWidth + root.extra : lbl.implicitWidth + 2 * Math.min(root.itemPadding, 10)
                radius: Theme.radius - 1
                color: index === root.currentIndex ? Theme.accent : (ma.containsMouse ? Theme.cardHover : "transparent")
                Label { id: lblBold; visible: false; text: parent.modelData; font.pixelSize: root.fontSize; font.weight: Font.DemiBold }
                Label {
                    id: lbl
                    anchors.centerIn: parent
                    text: parent.modelData
                    font.pixelSize: root.fontSize
                    font.weight: parent.index === root.currentIndex ? Font.DemiBold : Font.Normal
                    color: parent.index === root.currentIndex ? "white" : Theme.text2
                }
                MouseArea {
                    id: ma
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { root.currentIndex = parent.index; root.activated(parent.index) }
                }
            }
        }
    }
}
