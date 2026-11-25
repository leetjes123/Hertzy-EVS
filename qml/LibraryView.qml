import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: page
    spacing: Theme.gap
    readonly property var d: lib.detail

    // ---- grid of recordings ---------------------------------------------------------------------------------------------
    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.panel
        radius: Theme.radius
        Label {
            anchors.centerIn: parent
            visible: lib.clips.length === 0
            width: Math.min(parent.width - 40, 520)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: Theme.text3
            font.pixelSize: 16
            text: lib.totalClips === 0 ? "No recordings in " + (lib.project || "Unsorted") + " yet.\n" + (win.viewer ? "Copy .raw recordings into the library folder" : "Record on the Live page (2)") + ", or pick another project on the left."
                                       : "No recording matches the filter."
        }
        GridView {
            id: grid
            anchors.fill: parent
            anchors.margins: 4
            clip: true
            // a media bin: small thumbnails, as many columns as fit
            readonly property int cols: Math.max(1, Math.floor(width / 210))
            cellWidth: Math.floor(width / cols)
            cellHeight: (cellWidth - 8) * 9 / 16 + 50
            model: lib.clips
            ScrollBar.vertical: ScrollBar {}
            delegate: Item {
                required property var modelData
                width: grid.cellWidth
                height: grid.cellHeight
                Rectangle {
                    id: card
                    anchors.fill: parent
                    anchors.margins: 3
                    radius: Theme.radius
                    color: modelData.selected ? "#24344a" : (cm.containsMouse ? Theme.cardHover : "transparent")
                    border.color: modelData.selected ? Theme.accent : "transparent"
                    border.width: 1
                    ToolTip.visible: cm.containsMouse && !!modelData.description
                    ToolTip.delay: 700
                    ToolTip.text: modelData.description || ""
                    clip: true
                    MouseArea {
                        id: cm
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: lib.select(modelData.path)
                        onDoubleClicked: win.openClip(modelData.path)
                    }
                    Rectangle {
                        id: thumbBox
                        x: 1; y: 1
                        width: parent.width - 2
                        height: width * 9 / 16
                        radius: Theme.radius - 1
                        color: "#0c0e11"
                        Image {
                            anchors.fill: parent
                            source: modelData.thumb
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: false
                            smooth: true
                        }
                        BusyIndicator { anchors.centerIn: parent; running: !modelData.thumb; visible: running; width: 36; height: 36 }
                        // tick for a batch run: a small box on the thumbnail (shown on hover or when ticked)
                        Rectangle {
                            x: 6; y: 6
                            width: 18; height: 18
                            radius: Theme.radius
                            visible: modelData.checked || cm.containsMouse || tickMa.containsMouse
                            color: modelData.checked ? Theme.accent : "#a0101114"
                            border.color: modelData.checked ? Theme.accent : (tickMa.containsMouse ? "white" : "#c0ffffff")
                            Label { anchors.centerIn: parent; visible: modelData.checked; text: Theme.gCheck; font.family: Theme.icons; font.pixelSize: 11; color: "white" }
                            MouseArea {
                                id: tickMa
                                anchors.fill: parent
                                anchors.margins: -4
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: lib.toggleChecked(modelData.path)
                            }
                            ToolTip.visible: tickMa.containsMouse
                            ToolTip.delay: 600
                            ToolTip.text: "Tick for a batch run"
                        }
                        Row {
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 6
                            spacing: 4
                            Chip { visible: modelData.measurements > 0; text: modelData.measurements + (modelData.measurements === 1 ? " measurement" : " measurements"); tint: Theme.ok }
                            Chip { visible: modelData.triggers > 0; text: modelData.triggers + " trig"; tint: Theme.tap }
                        }
                        Label {
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 6
                            visible: modelData.duration > 0
                            text: modelData.duration.toFixed(2) + " s"
                            font.family: Theme.mono
                            font.pixelSize: Theme.fsSmall
                            color: "white"
                            style: Text.Outline
                            styleColor: "#c0000000"
                        }
                    }
                    Column {
                        anchors.top: thumbBox.bottom
                        anchors.topMargin: 5
                        x: 6
                        width: parent.width - 12
                        spacing: 1
                        Label { width: parent.width; text: modelData.name; color: Theme.text; font.weight: Font.DemiBold; elide: Text.ElideMiddle }
                        Label {
                            width: parent.width
                            text: modelData.day + "  " + modelData.time + "   ·   " + (modelData.events > 0 ? (modelData.events / 1e6).toFixed(1) + " M ev" : "–") + "   ·   " + modelData.mb.toFixed(0) + " MB"
                            color: Theme.text3
                            font.pixelSize: Theme.fsSmall
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }
    }

    // ---- details of the selected recording ------------------------------------------------------------------------------------
    Rectangle {
        Layout.preferredWidth: 340
        Layout.fillHeight: true
        visible: !!page.d.path
        radius: Theme.radius
        color: Theme.panel
        ScrollView {
            id: dsv
            anchors.fill: parent
            anchors.margins: 1
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Column {
                x: 14
                width: dsv.availableWidth - 28
                spacing: 10
                topPadding: 14
                bottomPadding: 20
                Rectangle {
                    width: parent.width
                    height: width * 9 / 16
                    radius: Theme.radius
                    color: "#0c0e11"
                    Image { anchors.fill: parent; anchors.margins: 1; source: page.d.thumb || ""; fillMode: Image.PreserveAspectFit; cache: false; smooth: true }
                }
                RowLayout {
                    width: parent.width
                    spacing: 4
                    Label { id: nameLbl; visible: !nameEdit.visible; Layout.fillWidth: true; text: page.d.name || ""; color: Theme.text; font.pixelSize: 14; font.weight: Font.DemiBold; elide: Text.ElideMiddle }
                    TextField {
                        id: nameEdit
                        visible: false
                        Layout.fillWidth: true
                        onAccepted: { if (lib.rename(page.d.path, text)) visible = false; win.releaseFocus() }
                        onActiveFocusChanged: if (!activeFocus) visible = false
                    }
                    IconButton { glyph: Theme.gEdit; size: 28; tip: "Rename (moves the .raw, .json, thumbnail and measurements together)"; onClicked: { nameEdit.text = page.d.name; nameEdit.visible = true; nameEdit.forceActiveFocus(); nameEdit.selectAll() } }
                }
                TextArea {
                    id: descEdit
                    width: parent.width
                    wrapMode: TextEdit.Wrap
                    placeholderText: "description: what was recorded, how it was excited, anything special"
                    text: page.d.description || ""
                    onEditingFinished: if (text !== (page.d.description || "")) lib.setDescription(page.d.path, text)
                }
                RowLayout {
                    width: parent.width
                    spacing: 6
                    Button { Layout.fillWidth: true; text: "Open in Analyse"; highlighted: true; onClicked: win.openClip(page.d.path) }
                    Button {
                        text: "Move to…"
                        onClicked: moveMenu.popup()
                        Menu {
                            id: moveMenu
                            Repeater {
                                model: lib.projects
                                MenuItem {
                                    required property var modelData
                                    text: modelData.label
                                    enabled: modelData.name !== lib.project
                                    onTriggered: lib.moveTo(page.d.path, modelData.name)
                                }
                            }
                        }
                    }
                    IconButton { glyph: Theme.gFolder; size: 32; tip: "Show in the file manager"; onClicked: lib.reveal(page.d.path) }
                    IconButton { glyph: Theme.gRestart; size: 32; tip: "Make the thumbnail and stats again"; onClicked: lib.regenerate(page.d.path) }
                    IconButton { glyph: Theme.gDelete; size: 32; tip: "Delete (moves the files to _trash)"; onClicked: delPop.open() }
                }
                // metadata
                Column {
                    width: parent.width
                    spacing: 3
                    Repeater {
                        model: page.d.rows || []
                        delegate: RowLayout {
                            required property var modelData
                            width: parent.width
                            spacing: 8
                            Label { Layout.preferredWidth: 110; Layout.alignment: Qt.AlignTop; text: modelData.k; color: Theme.text3; font.pixelSize: Theme.fsSmall }
                            Label { Layout.fillWidth: true; text: modelData.v; color: Theme.text; font.pixelSize: Theme.fsSmall; wrapMode: Text.WrapAnywhere }
                        }
                    }
                }
                // measurements
                Label { text: "Measurements"; color: Theme.text; font.weight: Font.DemiBold; topPadding: 4 }
                Label {
                    visible: !page.d.measurementList || page.d.measurementList.length === 0
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Theme.text3
                    font.pixelSize: Theme.fsSmall
                    text: "None yet. Open in Analyse, put ROIs on the map and save (Ctrl+S)."
                }
                Repeater {
                    model: page.d.measurementList || []
                    delegate: Rectangle {
                        required property var modelData
                        width: parent.width
                        height: mcol.implicitHeight + 16
                        radius: Theme.radius
                        color: Theme.field
                        Column {
                            id: mcol
                            x: 10; y: 8
                            width: parent.width - 20
                            spacing: 4
                            RowLayout {
                                width: parent.width
                                Label { Layout.fillWidth: true; text: modelData.name; color: Theme.text; font.weight: Font.DemiBold; elide: Text.ElideRight }
                                Button { text: "Open"; flat: true; onClicked: { ana.openMeasurement(page.d.path, modelData.dir); app.setv("page", "analyse") } }
                                IconButton { glyph: Theme.gFolder; size: 28; tip: "Show the files"; onClicked: lib.reveal(modelData.dir) }
                            }
                            Label { text: modelData.created + "   ·   " + modelData.window + "   ·   " + modelData.signal; color: Theme.text3; font.pixelSize: Theme.fsSmall; width: parent.width; elide: Text.ElideRight }
                            Image {
                                visible: modelData.hasMap
                                width: parent.width
                                height: visible ? width * 9 / 16 : 0
                                source: modelData.hasMap ? modelData.map : ""
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                cache: false
                                sourceSize.width: 640
                            }
                            Flow {
                                width: parent.width
                                spacing: 4
                                Repeater {
                                    model: modelData.results
                                    Chip {
                                        required property var modelData
                                        text: modelData.name + (modelData.f > 0 ? "  " + modelData.f.toFixed(1) + " Hz" : "  –")
                                        tint: modelData.ok ? Theme.ok : Theme.text3
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Popup {
        id: delPop
        anchors.centerIn: Overlay.overlay
        modal: true
        padding: 16
        background: Rectangle { color: Theme.card; radius: Theme.radius; border.color: Theme.borderStrong }
        Column {
            spacing: 10
            width: 360
            Label { text: "Delete " + (page.d.name || "") + "?"; color: Theme.text; font.weight: Font.DemiBold; width: parent.width; elide: Text.ElideMiddle }
            Hint { text: "The recording, its sidecar, thumbnail and measurements move to _trash in the library folder. Empty that folder yourself to free the disk." }
            RowLayout {
                width: parent.width
                Button { Layout.fillWidth: true; text: "Move to trash"; highlighted: true; palette.accent: Theme.err; onClicked: { lib.trash(page.d.path); delPop.close() } }
                Button { Layout.fillWidth: true; text: "Cancel"; onClicked: delPop.close() }
            }
        }
    }
}
