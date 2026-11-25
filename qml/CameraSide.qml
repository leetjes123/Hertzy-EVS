import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

PageScroll {
    id: page

    Card {
        title: "Stream"
        glyph: Theme.gCamera
        FormRow {
            label: "Event data"
            help: "Encoded: the camera's EVT 3.0 words, written to the file unchanged (smallest, lossless). Decoded: Arena decodes the events; the recorder re-encodes them to EVT 3.0 for the file. Use Decoded if Encoded shows garbage (bad words below)."
            Segmented { fill: true; model: ["Encoded EVT3", "Decoded"]; currentIndex: app.st.streamFmt === "decoded" ? 1 : 0; onActivated: (i) => app.setv("streamFmt", i === 1 ? "decoded" : "encoded") }
        }
        Label {
            width: parent.width
            font.family: Theme.mono
            font.pixelSize: Theme.fsSmall
            color: app.live.badWords > 0 ? Theme.warn : Theme.text2
            text: "bad words " + (app.live.badWords || 0) + "   ext. triggers " + (app.live.extTriggers || 0)
                  + "\ncamera rate " + Theme.fmt((app.live.cameraRate || 0) / 1e6, 2) + " Mev/s   host " + Theme.fmt((app.live.hostBps || app.live.linkBps || 0) / 1e6, 1) + " MB/s"
                  + "\nmissed packets " + (app.live.missedPackets || 0) + "   missed buffers " + (app.live.missedImages || 0) + "   lost " + (app.live.lost || 0)
                  + (app.live.temperature ? "\nsensor " + Theme.fmt(app.live.temperature, 1) + " °C" : "")
        }
        RowLayout {
            width: parent.width
            spacing: 6
            Button { Layout.fillWidth: true; text: "Re-send all settings"; onClicked: app.reapplyAll() }
            Button { Layout.fillWidth: true; text: "Save camera XML"; enabled: app.st.source === "camera"; onClicked: app.downloadXml() }
        }
    }

    Card {
        title: "Feature → camera node"
        glyph: Theme.gLink
        subtitle: "Found by name at connect. If a feature shows no node or an error, look up the right node in the list on the right and type its name here (Enter)."
        Repeater {
            model: app.features
            delegate: Column {
                required property var modelData
                width: parent.width
                spacing: 2
                RowLayout {
                    width: parent.width
                    spacing: 6
                    Label { text: modelData.label; color: Theme.text2; Layout.preferredWidth: 112; elide: Text.ElideRight }
                    TextField {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 30
                        font.family: Theme.mono
                        font.pixelSize: Theme.fsSmall
                        text: modelData.node
                        placeholderText: "not found"
                        // show the start of long node names (the device prefix is the useful part)
                        onTextChanged: if (!activeFocus) cursorPosition = 0
                        onActiveFocusChanged: if (!activeFocus) cursorPosition = 0
                        Component.onCompleted: cursorPosition = 0
                        ToolTip.visible: hovered && text.length > 0
                        ToolTip.delay: 500
                        ToolTip.text: text
                        onAccepted: { app.setNodeOverride(modelData.key, text); win.releaseFocus() }
                    }
                    Label { text: modelData.value; color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsSmall; Layout.preferredWidth: 60; elide: Text.ElideRight; horizontalAlignment: Text.AlignRight }
                }
                Label {
                    visible: modelData.error !== ""
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: modelData.error
                    color: Theme.warn
                    font.pixelSize: Theme.fsSmall
                }
            }
        }
    }
}
