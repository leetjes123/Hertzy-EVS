import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A fold-out section of a dock (After Effects "effect controls" style): a flat header strip with a twirl arrow, the
// title, an (i) with the help text and optional actions; the body below. No card: sections are separated by hairlines.
//   subtitle = help text (behind the (i); inline when Theme.showHelp)   info = short live status, always shown
Item {
    id: root
    property string title
    property string subtitle
    property string info
    property color infoColor: Theme.text2
    property string glyph                       // kept for compatibility, not drawn
    property bool collapsible: true
    property string foldKey: (app.st.page || "") + "/" + title
    property bool expanded: !Theme.folded(foldKey, false)
    property alias actions: actionRow.data
    default property alias content: body.data
    property int pad: 12
    width: parent ? parent.width : 300
    implicitHeight: col.implicitHeight + 1

    Column {
        id: col
        width: root.width
        // header strip
        Rectangle {
            visible: root.title !== ""
            width: parent.width
            height: Theme.headerH
            color: hdrMa.containsMouse && root.collapsible ? Theme.cardHover : Theme.card
            MouseArea {
                id: hdrMa
                anchors.fill: parent
                hoverEnabled: true
                enabled: root.collapsible
                cursorShape: root.collapsible ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: { root.expanded = !root.expanded; Theme.setFolded(root.foldKey, !root.expanded) }
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 6
                Label {
                    visible: root.collapsible
                    text: "▶"
                    font.pixelSize: 8
                    color: Theme.text2
                    rotation: root.expanded ? 90 : 0
                    Behavior on rotation { NumberAnimation { duration: 100 } }
                    Layout.preferredWidth: 10
                    horizontalAlignment: Text.AlignHCenter
                }
                Label {
                    text: root.title
                    font.pixelSize: Theme.fsTitle
                    font.weight: Font.DemiBold
                    color: Theme.text
                    elide: Text.ElideRight
                    Layout.fillWidth: implicitWidth > width
                    Layout.maximumWidth: implicitWidth
                    Layout.minimumWidth: 40
                    Layout.preferredWidth: implicitWidth
                }
                HelpBubble {
                    visible: root.subtitle !== "" && !Theme.showHelp
                    help: root.subtitle
                }
                Item { Layout.fillWidth: true }
                Row { id: actionRow; spacing: 2; Layout.alignment: Qt.AlignVCenter }
            }
        }
        Rectangle { width: parent.width; height: 1; color: Theme.bg }
        // body
        Item {
            width: parent.width
            visible: root.expanded || root.title === ""
            height: visible ? bodyCol.implicitHeight + 2 * root.pad - 2 : 0
            Column {
                id: bodyCol
                x: root.pad
                y: root.pad - 2
                width: parent.width - 2 * root.pad
                spacing: 8
                Label {
                    visible: root.info !== ""
                    width: parent.width
                    text: root.info
                    color: root.infoColor
                    font.pixelSize: Theme.fsSmall
                    wrapMode: Text.WordWrap
                }
                Label {
                    visible: root.subtitle !== "" && Theme.showHelp
                    width: parent.width
                    text: root.subtitle
                    color: Theme.text3
                    font.pixelSize: Theme.fsSmall
                    wrapMode: Text.WordWrap
                }
                Column {
                    id: body
                    width: parent.width
                    spacing: 8
                }
            }
        }
        Rectangle { width: parent.width; height: 1; color: Theme.bg; visible: root.expanded }
    }
}
