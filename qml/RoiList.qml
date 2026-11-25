import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// list of the ROIs of a controller (app or ana): select, rename, signal, line width, snap / duplicate / delete
Column {
    id: root
    property var ctl
    property var tones: ({})        // id -> {text, ok}
    property bool showSnap: false
    width: parent ? parent.width : 300
    spacing: 4

    readonly property var signalKeys: ["auto", "onoff", "on", "off", "count"]
    readonly property var signalNames: ["Auto", "ON−OFF", "ON", "OFF", "ON+OFF"]

    Label {
        visible: root.ctl.rois.length === 0
        width: parent.width
        wrapMode: Text.WordWrap
        color: Theme.text3
        font.pixelSize: Theme.fsSmall
        text: "No ROIs. Tools: L line (along an edge: sided ON−OFF), R rectangle, G polygon, B brush; V selects and moves, Del removes."
    }
    Repeater {
        model: root.ctl.rois
        delegate: Rectangle {
            id: row
            required property var modelData
            readonly property bool sel: modelData.id === root.ctl.selRoi
            width: root.width
            height: sel ? 36 + opts.implicitHeight + 6 : 36
            radius: Theme.radius
            color: sel ? Theme.accentSoft : (rm.containsMouse ? "#18ffffff" : "transparent")
            MouseArea { id: rm; anchors.fill: parent; hoverEnabled: true; onClicked: root.ctl.selectRoi(row.modelData.id) }
            RowLayout {
                x: 8
                width: parent.width - 10
                height: 36
                spacing: 6
                Rectangle { width: 10; height: 10; radius: row.modelData.type === "line" ? 1 : 5; color: row.modelData.color }
                TextField {
                    id: nameEdit
                    visible: row.sel
                    Layout.preferredWidth: 90
                    Layout.preferredHeight: 28
                    text: row.modelData.name
                    onEditingFinished: root.ctl.setRoiProp(row.modelData.id, "name", text)
                    onAccepted: win.releaseFocus()
                }
                Label { visible: !row.sel; text: row.modelData.name; font.weight: Font.DemiBold; color: Theme.text; Layout.preferredWidth: 46 }
                Label { text: row.modelData.type + (row.modelData.source === "lines" || row.modelData.source === "blobs" ? " · auto" : ""); color: Theme.text3; font.pixelSize: Theme.fsSmall }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignRight
                    font.family: Theme.mono
                    font.pixelSize: Theme.fsSmall
                    readonly property var t: root.tones[row.modelData.id]
                    color: t && t.ok ? Theme.ok : Theme.text3
                    text: t ? t.text : ""
                    elide: Text.ElideLeft
                }
                IconButton { visible: root.showSnap && row.modelData.type === "line"; glyph: Theme.gTarget; size: 28; tip: "Snap: centre the line on the edges that fire"; onClicked: root.ctl.snapRoi(row.modelData.id) }
                IconButton { glyph: Theme.gCopy; size: 28; tip: "Duplicate (Ctrl+D)"; onClicked: root.ctl.duplicateRoi(row.modelData.id) }
                IconButton { glyph: Theme.gDelete; size: 28; tip: "Remove (Del)"; onClicked: root.ctl.removeRoi(row.modelData.id) }
            }
            Column {
                id: opts
                visible: row.sel
                x: 8
                y: 38
                width: parent.width - 16
                spacing: 4
                FormRow {
                    label: "Signal"
                    labelWidth: 0.25
                    help: "Auto: a line counts ON−OFF with the two sides of opposite sign (an edge moving sideways adds up instead of cancelling); an area counts ON−OFF. ON or OFF alone: one edge direction. ON+OFF: activity, shows an oscillation also at 2f."
                    Segmented {
                        fill: true
                        fontSize: Theme.fsSmall
                        itemPadding: 6
                        model: root.signalNames
                        currentIndex: Math.max(0, root.signalKeys.indexOf(row.modelData.signal))
                        onActivated: (i) => root.ctl.setRoiProp(row.modelData.id, "signal", root.signalKeys[i])
                    }
                }
                NumberField {
                    visible: row.modelData.type === "line"
                    label: "Half-width"
                    value: row.modelData.half
                    unit: "px"
                    decimals: 1
                    step: 0.5
                    from: 1
                    to: 200
                    onEdited: (v) => root.ctl.setRoiProp(row.modelData.id, "half", v)
                    onAccepted: win.releaseFocus()
                    help: "Each side band reaches this far from the line: the edge plus its motion, about the object half-width + 3 px."
                }
                Label {
                    width: parent.width
                    font.family: Theme.mono
                    font.pixelSize: Theme.fsSmall
                    color: Theme.text3
                    text: row.modelData.type === "line" ? "length " + row.modelData.len.toFixed(0) + " px"
                        : "box " + row.modelData.bx.toFixed(0) + "," + row.modelData.by.toFixed(0) + "  " + row.modelData.bw.toFixed(0) + " × " + row.modelData.bh.toFixed(0) + " px"
                }
            }
        }
    }
}
