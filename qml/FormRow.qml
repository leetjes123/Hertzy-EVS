import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// label (+ optional help bubble) on the left, an editor on the right
Item {
    id: root
    property string label
    property string help
    property real labelWidth: 0.42
    default property alias editor: holder.data
    // inside a RowLayout the layout sizes the row (Layout.fillWidth); a parent.width binding would override it
    width: parent ? (parent instanceof RowLayout ? implicitWidth : parent.width) : 300
    implicitWidth: 200
    implicitHeight: Math.max(32, holder.children.length > 0 ? holder.children[0].implicitHeight : 32)

    Row {
        id: lab
        width: root.width * root.labelWidth - 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 5
        Label {
            id: lt
            text: root.label
            color: Theme.text2
            font.pixelSize: Theme.fs
            elide: Text.ElideRight
            width: Math.min(implicitWidth, lab.width - (root.help !== "" ? 18 : 0))
            anchors.verticalCenter: parent.verticalCenter
        }
        HelpBubble {
            visible: root.help !== ""
            help: root.help
            anchors.verticalCenter: parent.verticalCenter
        }
    }
    Item {
        id: holder
        x: root.width * root.labelWidth
        width: root.width - x
        height: root.height
        onChildrenChanged: {
            for (var i = 0; i < children.length; ++i) {
                children[i].width = Qt.binding(() => holder.width)
                children[i].anchors.verticalCenter = holder.verticalCenter
            }
        }
    }
}
