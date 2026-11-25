import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Panel {
    id: root
    title: "Camera nodes"
    subtitle: "Every feature of the camera's node map (device + stream). Enter on a value writes it to the camera."
    property var nodes: []
    function refresh() { nodes = app.nodeList(filter.text) }
    Component.onCompleted: refresh()
    Connections { target: app; function onFeaturesChanged() { refreshTimer.restart() } }
    Timer { id: refreshTimer; interval: 300; onTriggered: root.refresh() }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            TextField {
                id: filter
                Layout.fillWidth: true
                placeholderText: "Filter, e.g. bias, afk, flicker, roi, erc, trail, evs"
                onTextChanged: refreshTimer.restart()
            }
            Segmented {
                model: ["Bias", "Flicker", "ROI", "Erc", "All"]
                currentIndex: -1
                onActivated: (i) => { filter.text = ["bias", "flicker", "roi", "erc", ""][i] }
            }
            IconButton { glyph: Theme.gRestart; tip: "Read again"; onClicked: root.refresh() }
        }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.nodes
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                required property var modelData
                required property int index
                width: ListView.view.width
                height: 38
                color: index % 2 ? "transparent" : "#0dffffff"
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 10
                    Label {
                        Layout.preferredWidth: 300
                        text: modelData.name
                        color: Theme.text
                        font.family: Theme.mono
                        font.pixelSize: Theme.fsSmall
                        elide: Text.ElideRight
                        ToolTip.visible: nm.containsMouse && modelData.tip !== ""
                        ToolTip.text: modelData.display + (modelData.tip ? "\n" + modelData.tip : "")
                        MouseArea { id: nm; anchors.fill: parent; hoverEnabled: true }
                    }
                    Label { Layout.preferredWidth: 70; text: modelData.type; color: Theme.text3; font.pixelSize: Theme.fsSmall }
                    TextField {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 30
                        enabled: modelData.writable
                        font.family: Theme.mono
                        font.pixelSize: Theme.fsSmall
                        text: modelData.value
                        onAccepted: { app.setNode(modelData.name, text); win.releaseFocus() }
                    }
                    Label {
                        Layout.preferredWidth: 220
                        color: Theme.text3
                        font.pixelSize: Theme.fsSmall
                        elide: Text.ElideRight
                        text: modelData.entries && modelData.entries.length ? modelData.entries.join(" | ")
                              : (modelData.type === "Integer" || modelData.type === "Float") && modelData.min !== modelData.max ? modelData.min + " … " + modelData.max : ""
                    }
                }
            }
        }
    }
}
