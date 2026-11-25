import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

PageScroll {
    id: page

    Card {
        title: "Projects"
        glyph: Theme.gFolder
        subtitle: "A project is a folder in the library." + (win.viewer ? "" : " New recordings go to the project chosen on the Live page.")
        Repeater {
            model: lib.projects
            delegate: Rectangle {
                required property var modelData
                readonly property bool sel: modelData.name === lib.project
                width: parent.width
                height: 34
                radius: Theme.radius
                color: sel ? Theme.accentSoft : (pm.containsMouse ? "#18ffffff" : "transparent")
                visible: modelData.name !== "" || modelData.count > 0 || sel
                MouseArea { id: pm; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: lib.setProject(modelData.name) }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    Label { text: modelData.name === "" ? Theme.gList : Theme.gFolder; font.family: Theme.icons; color: sel ? Theme.accent : Theme.text3 }
                    Label { Layout.fillWidth: true; text: modelData.label; color: Theme.text; font.weight: sel ? Font.DemiBold : Font.Normal; font.italic: modelData.name === ""; elide: Text.ElideRight }
                    Label { text: modelData.count; color: Theme.text3; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                }
            }
        }
        RowLayout {
            width: parent.width
            spacing: 6
            TextField { id: newProj; Layout.fillWidth: true; placeholderText: "new project name"; onAccepted: if (lib.createProject(text)) { text = ""; win.releaseFocus() } }
            Button { text: "Create"; enabled: newProj.text.trim().length > 0; onClicked: newProj.accepted() }
        }
        Label { text: "Project notes"; color: Theme.text2; font.pixelSize: Theme.fsSmall; visible: lib.project !== "" }
        TextArea {
            visible: lib.project !== ""
            width: parent.width
            wrapMode: TextEdit.Wrap
            placeholderText: "what this project is about, setup, conditions…"
            text: lib.projectDescription
            onEditingFinished: if (text !== lib.projectDescription) lib.setProjectDescription(text)
        }
    }

    Card {
        title: "Find"
        glyph: Theme.gView
        TextField {
            width: parent.width
            placeholderText: "filter by name or description"
            text: lib.filter
            onTextChanged: lib.setFilter(text)
        }
        FormRow {
            label: "Sort"
            labelWidth: 0.2
            Segmented {
                fill: true
                model: ["Newest", "Name", "Size", "Measured"]
                readonly property var keys: ["date", "name", "size", "measured"]
                currentIndex: Math.max(0, keys.indexOf(lib.sort))
                onActivated: (i) => lib.setSort(keys[i])
            }
        }
        Label {
            width: parent.width
            color: Theme.text3
            font.pixelSize: Theme.fsSmall
            text: lib.clips.length + " of " + lib.totalClips + " recordings" + (lib.pendingStats > 0 ? "   ·   making thumbnails: " + lib.pendingStats + " to go" : "")
        }
    }

    Card {
        title: "Batch"
        glyph: Theme.gLayers
        subtitle: "Run one saved measurement (its ROIs, window rule and settings) on every ticked recording. Each gets the measurement saved under the batch name; one CSV collects all tones."
        RowLayout {
            width: parent.width
            Label { Layout.fillWidth: true; text: lib.checked.length + " recordings ticked"; color: lib.checked.length ? Theme.text : Theme.text3 }
            Button { text: "All"; onClicked: lib.checkAll(true) }
            Button { text: "None"; enabled: lib.checked.length > 0; onClicked: lib.checkAll(false) }
        }
        FormRow {
            label: "Recipe"
            labelWidth: 0.22
            ComboBox {
                id: recipeBox
                property var items: []
                function reload() { items = lib.recipes(); model = items.map(r => r.label) }
                Component.onCompleted: reload()
                Connections { target: lib; function onProjectChanged() { recipeBox.reload() } function onClipsChanged() { recipeBox.reload() } }
                displayText: items.length ? currentText : "no saved measurements in this project"
            }
        }
        Hint { help: true; text: "A recipe with a window \"from onset\" follows each recording's own tap or pluck; an absolute window uses the same seconds everywhere." }
        FormRow {
            label: "Save as"
            labelWidth: 0.22
            TextField { id: batchName; placeholderText: recipeBox.items.length ? recipeBox.items[Math.max(0, recipeBox.currentIndex)].name : "name" }
        }
        RowLayout {
            width: parent.width
            spacing: 6
            Button {
                Layout.fillWidth: true
                text: lib.batch.running ? "Running " + lib.batch.done + " / " + lib.batch.total : "Run on " + lib.checked.length + " recordings"
                highlighted: true
                enabled: !lib.batch.running && lib.checked.length > 0 && recipeBox.items.length > 0
                onClicked: lib.runBatch(recipeBox.items[recipeBox.currentIndex].dir, batchName.text)
            }
            Button { text: "Stop"; visible: lib.batch.running === true; onClicked: lib.cancelBatch() }
        }
        ProgressBar { visible: lib.batch.running === true; width: parent.width; value: lib.batch.total ? lib.batch.done / lib.batch.total : 0 }
        Label { visible: !!lib.batch.current; text: lib.batch.current || ""; color: Theme.text3; font.pixelSize: Theme.fsSmall; elide: Text.ElideMiddle; width: parent.width }
        Repeater {
            model: lib.batch.rows || []
            delegate: Label {
                required property var modelData
                width: parent.width
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fsSmall
                color: modelData.note ? Theme.warn : Theme.text2
                text: modelData.clip + ":  " + (modelData.note ? modelData.note : (modelData.ok + "/" + modelData.rois + " tones  " + modelData.tones))
            }
        }
        RowLayout {
            visible: !!lib.batch.csv && !lib.batch.running
            width: parent.width
            Label { Layout.fillWidth: true; text: lib.batch.csv || ""; color: Theme.text3; font.pixelSize: Theme.fsSmall; elide: Text.ElideLeft }
            IconButton { glyph: Theme.gFolder; size: 28; tip: "Show the CSV"; onClicked: lib.reveal(lib.batch.csv) }
        }
    }

    Card {
        title: "Library folder"
        glyph: Theme.gSave
        Label { width: parent.width; text: lib.root; color: Theme.text2; font.pixelSize: Theme.fsSmall; wrapMode: Text.WrapAnywhere }
        RowLayout {
            width: parent.width
            spacing: 6
            Button { Layout.fillWidth: true; text: "Change…"; onClicked: rootDlg.open() }
            Button { Layout.fillWidth: true; text: "Open"; onClicked: lib.reveal(lib.root) }
            Button { Layout.fillWidth: true; text: "Rescan"; onClicked: lib.refresh() }
        }
        Hint { help: true; text: "Everything lives in plain files here: <clip>.raw + .json (settings, description, stats), .thumb.png and <clip>.measure/<name>/ (JSON, CSV, PNG). Deleted recordings go to _trash. Copy results off with scp." }
    }

    FolderDialog {
        id: rootDlg
        title: "Library folder"
        currentFolder: lib.pathToUrl(lib.root)
        onAccepted: lib.setRoot(lib.urlToPath(selectedFolder))
    }
}
