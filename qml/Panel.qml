import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A docked pane (After Effects / OpenShot style): flat body, a slim header strip with the title, an (i) holding the
// help text (inline when Theme.showHelp), a short live status (info) and optional header controls (tools).
Rectangle {
    id: root
    property string title
    property string subtitle            // help
    property string info                // live status, always shown in the header
    property alias tools: toolRow.data
    default property alias content: body.data
    color: Theme.panel
    radius: Theme.radius
    clip: true
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            visible: root.title !== ""
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.headerH
            color: Theme.card
            radius: Theme.radius
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.radius; color: parent.color }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 6
                spacing: 8
                Label {
                    text: root.title
                    font.pixelSize: Theme.fsTitle
                    font.weight: Font.DemiBold
                    color: Theme.text
                    elide: Text.ElideRight
                    Layout.preferredWidth: implicitWidth
                    Layout.minimumWidth: 30
                }
                HelpBubble { visible: root.subtitle !== "" && !Theme.showHelp; help: root.subtitle }
                Label {
                    visible: root.info !== ""
                    text: root.info
                    color: Theme.text2
                    font.pixelSize: Theme.fsSmall
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 20
                }
                Item { visible: root.info === ""; Layout.fillWidth: true }
                Row { id: toolRow; spacing: 4; Layout.alignment: Qt.AlignVCenter }
            }
        }
        Label {
            visible: root.subtitle !== "" && Theme.showHelp
            text: root.subtitle
            color: Theme.text3
            font.pixelSize: Theme.fsSmall
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            Layout.topMargin: 4
        }
        Item {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 6
        }
    }
}
