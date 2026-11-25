import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One slim bar: app mark, the page tabs (workspaces, keys 1-4), the camera status where it matters, and the
// side-panel / help toggles.
ToolBar {
    id: root
    property var pages: []
    property bool sideOpen: true
    signal toggleSide()
    height: 38
    background: Rectangle {
        color: Theme.rail
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.bg }
    }
    function mev(v) { return v === undefined || isNaN(v) ? "–" : (v / 1e6).toFixed(v < 1e6 ? 2 : 1) }
    readonly property bool camPage: app.st.page === "live" || app.st.page === "camera"
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 8
        spacing: 0
        Rectangle {
            width: 22; height: 22; radius: 5
            gradient: Gradient {
                GradientStop { position: 0; color: "#ff7ab8" }
                GradientStop { position: 1; color: "#c2367a" }
            }
            Label { anchors.centerIn: parent; text: Theme.gBolt; font.family: Theme.icons; font.pixelSize: 12; color: "white" }
        }
        Label {
            text: win.appName
            font.pixelSize: 13
            font.weight: Font.DemiBold
            color: Theme.text
            Layout.leftMargin: 8
            Layout.rightMargin: 14
        }
        IconButton {
            glyph: root.sideOpen ? Theme.gPaneClose : Theme.gPaneOpen
            tip: (root.sideOpen ? "Hide" : "Show") + " the side panel  [\\]"
            size: 28
            onClicked: root.toggleSide()
            Layout.rightMargin: 6
        }
        // page tabs
        Repeater {
            model: root.pages
            delegate: Item {
                required property var modelData
                required property int index
                readonly property bool sel: app.st.page === modelData.key
                Layout.fillHeight: true
                implicitWidth: tabRow.implicitWidth + 24
                Rectangle { anchors.fill: parent; color: tabMa.containsMouse && !parent.sel ? "#10ffffff" : "transparent" }
                Row {
                    id: tabRow
                    anchors.centerIn: parent
                    spacing: 7
                    Label {
                        text: modelData.glyph
                        font.family: Theme.icons
                        font.pixelSize: 13
                        color: sel ? Theme.accent : Theme.text3
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Label {
                        text: modelData.title
                        font.pixelSize: Theme.fs
                        font.weight: sel ? Font.DemiBold : Font.Normal
                        color: sel ? Theme.text : Theme.text2
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                Rectangle { visible: sel; anchors.bottom: parent.bottom; width: parent.width; height: 2; color: Theme.accent }
                MouseArea {
                    id: tabMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: app.setv("page", modelData.key)
                }
                ToolTip.visible: tabMa.containsMouse
                ToolTip.delay: 600
                ToolTip.text: modelData.title + "  [" + (index + 1) + "]"
            }
        }
        Item { Layout.fillWidth: true }
        // camera status: only on the pages that use the camera
        Label {
            visible: !win.viewer && (root.camPage || !app.live.connected)
            text: app.live.connected ? app.live.source : (app.live.error || "No camera")
            color: app.live.connected ? Theme.text3 : Theme.err
            font.pixelSize: Theme.fsSmall
            elide: Text.ElideRight
            Layout.maximumWidth: 300
            Layout.rightMargin: 12
        }
        Label {
            visible: root.camPage
            font.family: Theme.mono
            font.pixelSize: Theme.fsSmall
            color: Theme.text2
            text: root.mev(app.live.rate) + " Mev/s  link " + Theme.fmt(app.live.linkPct, 0) + " %"
                  + (app.live.queueMB > 1 ? "  queue " + Theme.fmt(app.live.queueMB, 0) + " MB" : "")
                  + (app.live.lost > 0 ? "  lost " + app.live.lost : "")
                  + (app.st.source === "sim" && app.live.realtime !== undefined ? "  sim ×" + Theme.fmt(app.live.realtime, 2) : "")
            Layout.rightMargin: 10
        }
        Rectangle {
            visible: !win.viewer
            Layout.preferredHeight: 22
            Layout.preferredWidth: statusRow.implicitWidth + 16
            radius: Theme.radius
            readonly property color tint: app.live.recColor || Theme.text3
            color: Qt.rgba(tint.r, tint.g, tint.b, 0.16)
            RowLayout {
                id: statusRow
                anchors.centerIn: parent
                spacing: 6
                Rectangle {
                    width: 7; height: 7; radius: 3.5; color: parent.parent.tint
                    SequentialAnimation on opacity {
                        running: app.live.recState === 2 || app.live.recState === 1
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.25; duration: 350 }
                        NumberAnimation { to: 1; duration: 350 }
                    }
                }
                Label { text: app.live.recText || ""; color: Theme.text; font.pixelSize: Theme.fsSmall; font.weight: Font.DemiBold }
            }
        }
        IconButton {
            glyph: Theme.gHelp
            tip: Theme.showHelp ? "Hide the help texts (they stay behind the ⓘ)" : "Show the help texts in the panels"
            active: Theme.showHelp
            size: 28
            onClicked: Theme.showHelp = !Theme.showHelp
            Layout.leftMargin: 6
        }
    }
}
