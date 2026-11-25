import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import HertzyEvs

ColumnLayout {
    id: page
    spacing: Theme.gap
    readonly property bool rpmOn: app.st.rpmOn === true
    // the ROI that gives the live speed: the selected one, else the first
    readonly property int rpmRoiId: app.rois.length === 0 ? -1 : (app.rois.find(r => r.id === app.selRoi) ? app.selRoi : app.rois[0].id)
    readonly property var labelMap: {
        const m = {}
        for (const t of app.liveTones) if (t.ok) m[t.id] = t.f.toFixed(1) + " Hz"
        if (page.rpmOn && page.rpmRoiId >= 0 && app.live.rpmOk === true) m[page.rpmRoiId] = app.live.rpm.toFixed(app.live.rpm >= 1000 ? 0 : 1) + " rpm"
        return m
    }

    // ---- tool bar -------------------------------------------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 30
        spacing: 6
        ToolStrip {
            tools: ["select", "line", "rect", "polygon", "brush", "erase", "pan", "crop"]
            current: app.st.tool
            onPicked: (t) => app.setv("tool", t)
        }
        NumberField {
            visible: app.st.tool === "brush" || app.st.tool === "erase"
            Layout.preferredWidth: 130
            label: "Size"
            labelWidth: 0.38
            value: app.st.brushR
            unit: "px"
            decimals: 0
            step: 1
            from: 1
            to: 80
            onEdited: (v) => app.setv("brushR", v)
            onAccepted: win.releaseFocus()
        }
        Segmented {
            model: ["Events", "Activity"]
            currentIndex: app.st.viewMode
            onActivated: (i) => app.setv("viewMode", i)
            itemPadding: 6
            fontSize: Theme.fsSmall
        }
        NumberField {
            Layout.preferredWidth: 150
            label: "Fade"
            labelWidth: 0.42
            value: app.st.decayMs
            unit: "ms"
            decimals: 1
            step: 1
            from: 0.1
            to: 1000
            onEdited: (v) => app.setv("decayMs", v)
            onAccepted: win.releaseFocus()
            help: "Events view: how long an event stays visible. Activity view: memory = 50 × this."
        }
        IconButton { glyph: Theme.gZoomFit; tip: "Fit the sensor (double-click)"; onClicked: lv.fit() }
        IconButton { glyph: Theme.gView; tip: "Zoom to the crop"; visible: app.st.roiOn === true; onClicked: lv.zoomTo(app.st.roiX, app.st.roiY, app.st.roiX + app.st.roiW, app.st.roiY + app.st.roiH) }
        Item { Layout.fillWidth: true }
        Button { visible: app.st.source === "sim"; text: "Tap"; onClicked: app.simTap() }
        Button {
            text: app.live.recState === 1 ? "Disarm" : "Arm"
            enabled: app.live.recState <= 1
            onClicked: app.toggleArm()
        }
        Button {
            text: app.live.recState >= 2 ? "Stop" : "● Record"
            highlighted: true
            onClicked: app.live.recState >= 2 ? app.cancelRecording() : app.recordNow()
            palette.accent: app.live.recState >= 2 ? Theme.err : "#e0457b"
        }
    }

    // ---- live view --------------------------------------------------------------------------------------------------
    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredHeight: 3
        color: "#0c0e11"
        radius: Theme.radius
        border.color: app.live.recState === 2 ? Theme.err : app.live.recState === 1 ? Theme.warn : app.st.amOn ? Theme.ok : Theme.border
        border.width: app.live.recState >= 1 || app.st.amOn ? 2 : 1
        clip: true

        EventView {
            id: lv
            objectName: "liveView"
            anchors.fill: parent
            anchors.margins: 2
            source: app
        }
        RoiLayer {
            id: layer
            anchors.fill: lv
            view: lv
            ctl: app
            tool: app.st.tool
            brushR: app.st.brushR
            labels: page.labelMap
            showCrop: app.st.roiOn === true
            crop: Qt.rect(app.st.roiX, app.st.roiY, app.st.roiW, app.st.roiH)
            onCropDrawn: (x0, y0, x1, y1) => app.setCrop(x0, y0, x1, y1)
        }
        // HUD
        Rectangle {
            x: 10; y: 10
            width: hud.implicitWidth + 16
            height: hud.implicitHeight + 10
            radius: Theme.radius
            color: "#b0111317"
            Label {
                id: hud
                x: 8; y: 5
                font.family: Theme.mono
                font.pixelSize: Theme.fsSmall
                color: Theme.text2
                text: "x " + layer.hoverImg.x.toFixed(0) + "  y " + layer.hoverImg.y.toFixed(0) + "   zoom ×" + lv.zoom.toFixed(2) + "   view " + lv.fps.toFixed(0) + " fps"
            }
        }
        // live speed + last auto-measure result, big, top right
        Column {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 10
            spacing: 8
        RpmReadout {
            anchors.right: parent.right
            visible: page.rpmOn
            ok: app.live.rpmOk === true
            rpm: app.live.rpm || -1
            roi: app.live.rpmRoi || ""
            roiColor: app.live.rpmColor || Theme.accent
            passHz: app.live.rpmPass || 0
            snr: app.live.rpmSnr || 0
            marks: app.st.rpmMarks
            note: app.live.rpmNote || ""
            waiting: app.rois.length === 0 ? "draw a ROI where the rotating part passes" : "no speed in " + Theme.fmt(app.st.rpmLo, 0) + "–" + Theme.fmt(app.st.rpmHi, 0) + " rpm"
        }
        Rectangle {
            visible: app.st.amOn === true
            anchors.right: parent.right
            width: amCol.implicitWidth + 28
            height: amCol.implicitHeight + 18
            radius: Theme.radius
            color: "#d0111317"
            border.color: app.lastAm.ok ? Theme.ok : Theme.border
            Column {
                id: amCol
                x: 14; y: 9
                spacing: 2
                Label { text: app.amCount === 0 ? "Auto-measure: waiting for a trigger" : "#" + app.lastAm.n + "   " + app.lastAm.time; color: Theme.text3; font.pixelSize: Theme.fsSmall }
                Label {
                    visible: app.amCount > 0
                    text: app.lastAm.empty ? "no ROIs" : app.lastAm.ok ? app.lastAm.best + "  " + app.lastAm.f.toFixed(2) + " Hz" : "no tone above the SNR limit"
                    color: app.lastAm.ok ? app.lastAm.color : Theme.warn
                    font.pixelSize: 28
                    font.weight: Font.DemiBold
                }
                Label { visible: app.lastAm.ok === true; text: "SNR " + Theme.fmt(app.lastAm.snr, 0); color: Theme.text2; font.pixelSize: Theme.fsSmall }
            }
        }
        }
        Label {
            anchors.centerIn: parent
            visible: !app.live.running
            text: app.live.connected ? "Stream stopped" : (app.live.error || "No camera")
            color: Theme.text3
            font.pixelSize: 18
        }
    }

    // ---- plots / auto-measure log -------------------------------------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredHeight: 2
        spacing: Theme.gap
        Panel {
            visible: page.rpmOn && !app.st.amOn
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 3
            title: "Speed"
            info: app.live.rpmOk === true ? app.live.rpm.toFixed(1) + " rpm" : ""
            subtitle: "RPM of " + (app.live.rpmRoi || "the selected ROI") + " over the last " + Theme.fmt(app.st.rpmHistS, 0) + " s (one point per " + Theme.fmt(app.st.rpmWin, 1) + " s window, 10 per second)"
            Plot { anchors.fill: parent; anchors.leftMargin: -6; source: app; kind: "live:rpm" }
        }
        Panel {
            visible: !app.st.amOn && !page.rpmOn
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 3
            title: "ROI signals"
            subtitle: "Last 0.3 s per ROI (" + (app.st.liveSignal === "displacement" ? "integrated, band-passed" : "ON−OFF rate") + ", relative)"
            Plot { anchors.fill: parent; anchors.leftMargin: -6; source: app; kind: "live:trace" }
        }
        Panel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 4
            title: "Spectrum (rolling " + Theme.fmt(app.st.liveWin, 1) + " s)"
            subtitle: "Dashed = tone search band " + Theme.fmt(app.st.toneLo, 0) + "–" + Theme.fmt(app.st.toneHi, 0) + " Hz; labels = tones above SNR " + Theme.fmt(app.st.snrMin, 0)
            Plot { anchors.fill: parent; anchors.leftMargin: -6; source: app; kind: "live:spectrum" }
        }
        Panel {
            visible: app.st.amOn === true
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 5
            title: "Auto-measure log"
            subtitle: "Newest first: the ROI that rang hardest, and the other ROIs with a tone"
            DataTable { anchors.fill: parent; model: app.amTable }
        }
        Panel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 2
            title: "Event rate"
            info: app.live.recState === 1 ? "Armed: waiting for the jump" : ""
            subtitle: "All pixels and the trigger region"
            Plot { anchors.fill: parent; anchors.leftMargin: -6; source: app; kind: "live:rate" }
        }
    }
}
