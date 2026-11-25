import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import HertzyEvs

ColumnLayout {
    id: page
    spacing: Theme.gap
    readonly property var c: ana.cfg
    readonly property bool playMode: player.cfg.active === true
    readonly property bool rpmOn: page.c.rpmOn === true
    // result of the selected ROI (else the first)
    readonly property var selRes: ana.results.find(r => r.id === ana.selRoi) || ana.results[0] || null
    readonly property var labelMap: {
        const m = {}
        if (page.playMode) { for (const r of player.tones) if (r.ok) m[r.id] = r.f.toFixed(1) + " Hz" }
        else for (const r of ana.results) {
            if (page.rpmOn && r.rpmOk) m[r.id] = r.rpm.toFixed(r.rpm >= 1000 ? 0 : 1) + " rpm"
            else if (r.ok) m[r.id] = r.f.toFixed(1) + " Hz"
        }
        return m
    }
    // speed tool: two clicks on the picture -> distance / time between the newest events there
    property var spA: null
    property var spB: null
    property var pix: ({})

    // ---- tool bar ---------------------------------------------------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 30
        spacing: 6
        ToolStrip {
            current: page.c.tool
            onPicked: (t) => ana.setCfg("tool", t)
        }
        NumberField {
            visible: page.c.tool === "brush" || page.c.tool === "erase"
            Layout.preferredWidth: 130
            label: "Size"
            labelWidth: 0.38
            value: page.c.brushR
            unit: "px"
            decimals: 0
            step: 1
            from: 1
            to: 80
            onEdited: (v) => ana.setCfg("brushR", v)
            onAccepted: win.releaseFocus()
        }
        Rectangle { width: 1; Layout.preferredHeight: 18; color: Theme.border }
        Segmented {
            model: ["Maps", "▶  Playback"]
            currentIndex: page.playMode ? 1 : 0
            onActivated: (i) => player.setv("active", i === 1)
            itemPadding: 8
            fontSize: Theme.fsSmall
        }
        Segmented {
            visible: !page.playMode
            model: ["Activity", "ON/OFF", "Frequency", "Energy"]
            readonly property var keys: ["activity", "polarity", "freq", "energy"]
            currentIndex: Math.max(0, keys.indexOf(page.c.map))
            onActivated: (i) => ana.setCfg("map", keys[i])
            itemPadding: 6
            fontSize: Theme.fsSmall
        }
        IconButton { glyph: Theme.gZoomFit; tip: "Fit (double-click)"; onClicked: mv.fit() }
        BusyIndicator { running: ana.state.busy === true || ana.state.loading === true; visible: running; Layout.preferredWidth: 22; Layout.preferredHeight: 22 }
        Item { Layout.fillWidth: true }
        Label {
            text: ana.state.measurement ? "“" + ana.state.measurement + "”" + (ana.state.dirty ? " (changed)" : "") : (ana.state.dirty ? "unsaved" : "")
            color: Theme.text3
            font.pixelSize: Theme.fsSmall
            elide: Text.ElideRight
            Layout.maximumWidth: 220
        }
        Button {
            text: "Save"
            highlighted: !!ana.state.dirty
            enabled: !!ana.clip.loaded && ana.rois.length > 0
            onClicked: win.saveMeasurementDialog()
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: "Save this measurement  [Ctrl+S]"
        }
    }

    // ---- map + ROIs ------------------------------------------------------------------------------------------------------------
    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredHeight: 5
        color: "#0c0e11"
        radius: Theme.radius
        border.color: Theme.border
        clip: true
        EventView {
            id: mv
            objectName: "anaView"
            anchors.fill: parent
            anchors.margins: 2
            source: page.playMode ? player : ana
        }
        RoiLayer {
            id: layer
            anchors.fill: mv
            view: mv
            ctl: ana
            tool: page.c.tool
            brushR: page.c.brushR
            labels: page.labelMap
            onHovered: (x, y) => page.pix = ana.pixelInfo(x, y)
        }
        // speed of the selected ROI over the analysis window
        RpmReadout {
            visible: page.rpmOn && !!ana.clip.loaded && !page.playMode && ana.rois.length > 0
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            ok: !!page.selRes && page.selRes.rpmOk === true
            rpm: page.selRes ? page.selRes.rpm : -1
            roi: page.selRes ? page.selRes.name : ""
            roiColor: page.selRes ? page.selRes.color : Theme.accent
            passHz: page.selRes ? page.selRes.rpmPass : 0
            snr: page.selRes ? page.selRes.rpmSnr : 0
            marks: page.c.rpmMarks
            note: page.selRes ? (page.selRes.rpmNote || "") : ""
            waiting: ana.state.busy ? "computing…" : "no speed in " + Theme.fmt(page.c.rpmLo, 0) + "–" + Theme.fmt(page.c.rpmHi, 0) + " rpm"
        }
        // playback HUD
        Rectangle {
            visible: page.playMode && player.loaded
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            width: phud.implicitWidth + 28
            height: phud.implicitHeight + 18
            radius: Theme.radius
            color: "#c80a0b0e"
            Column {
                id: phud
                x: 14; y: 9
                spacing: 2
                Label {
                    text: player.cfg.strobe ? "STROBE  " + Theme.fmt(player.cfg.strobeHz, 2) + " Hz"
                        : player.cfg.speed < 1 ? "SLOW MOTION  ×" + Math.round(1 / player.cfg.speed).toLocaleString(Qt.locale("en_US"), "f", 0)
                        : "SPEED  ×" + player.cfg.speed
                    color: "#ff4fb0"
                    font.pixelSize: 20
                    font.weight: Font.Bold
                }
                Label {
                    text: player.cfg.mode === "trail" ? "trail " + player.cfg.trailMs + " ms  ·  colour = time since the last event"
                        : "exposure " + (player.cfg.exposureUs >= 1000 ? player.cfg.exposureUs / 1000 + " ms" : player.cfg.exposureUs + " µs")
                          + "  =  " + Math.round(1e6 / player.cfg.exposureUs).toLocaleString(Qt.locale("en_US"), "f", 0) + " fps camera"
                    color: Theme.text
                    font.pixelSize: Theme.fs
                }
                Label {
                    text: (player.cfg.strobe ? "drift " + player.cfg.strobeDrift + " % per frame  ·  " : "") + "t = " + (player.t * 1000).toFixed(3) + " ms"
                    color: Theme.text2
                    font.family: Theme.mono
                    font.pixelSize: Theme.fsSmall
                }
            }
        }
        MouseArea {
            id: speedArea
            anchors.fill: mv
            visible: page.playMode && pbar.speedTool
            cursorShape: Qt.CrossCursor
            onClicked: (m) => {
                const p = mv.toImage(m.x, m.y)
                const q = player.timeAt(p.x, p.y)
                if (!q.ok) { toast.show("No event near this point in the trail: use Trail mode and a longer trail"); return }
                const pt = {x: p.x, y: p.y, t: q.t}
                if (!page.spA || page.spB) { page.spA = pt; page.spB = null } else page.spB = pt
            }
        }
        Canvas {
            id: spCanvas
            anchors.fill: mv
            visible: page.playMode && page.spA !== null
            property var res: {
                if (!page.spA || !page.spB) return null
                const d = Math.hypot(page.spB.x - page.spA.x, page.spB.y - page.spA.y), dt = Math.abs(page.spB.t - page.spA.t)
                return {d: d, dt: dt, v: dt > 0 ? d / dt : 0}
            }
            onResChanged: requestPaint()
            Connections { target: mv; function onViewChanged() { spCanvas.requestPaint() } }
            Connections { target: page; function onSpAChanged() { spCanvas.requestPaint() } }
            onPaint: {
                const g = getContext("2d")
                g.reset()
                if (!page.spA) return
                const a = mv.toItem(page.spA.x, page.spA.y)
                g.strokeStyle = "#ffe14d"; g.fillStyle = "#ffe14d"; g.lineWidth = 2
                g.beginPath(); g.arc(a.x, a.y, 5, 0, 2 * Math.PI); g.fill()
                if (!page.spB) return
                const b = mv.toItem(page.spB.x, page.spB.y)
                g.beginPath(); g.moveTo(a.x, a.y); g.lineTo(b.x, b.y); g.stroke()
                g.beginPath(); g.arc(b.x, b.y, 5, 0, 2 * Math.PI); g.fill()
                const r = res
                const txt = r.dt > 0 ? Math.round(r.v).toLocaleString(Qt.locale("en_US"), "f", 0) + " px/s   (" + r.d.toFixed(0) + " px in " + (r.dt * 1000).toFixed(3) + " ms)" : "same time stamp"
                g.font = "bold 15px sans-serif"
                const w = g.measureText(txt).width + 16
                const mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2 - 18
                g.fillStyle = "rgba(10,11,14,0.85)"; g.fillRect(mx - w / 2, my - 14, w, 24)
                g.fillStyle = "#ffe14d"; g.fillText(txt, mx - w / 2 + 8, my + 4)
            }
        }
        Rectangle {
            x: 10; y: 10
            visible: !!ana.clip.loaded
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
                text: "x " + layer.hoverImg.x.toFixed(0) + "  y " + layer.hoverImg.y.toFixed(0)
                      + (page.pix.on !== undefined ? "   ON " + page.pix.on + "  OFF " + page.pix.off : "")
                      + (page.pix.f !== undefined ? "   " + page.pix.f.toFixed(1) + " Hz  SNR " + page.pix.snr.toFixed(0) : "")
                      + "   zoom ×" + mv.zoom.toFixed(2)
            }
        }
        // frequency legend
        Rectangle {
            visible: page.c.map === "freq" && !!ana.clip.loaded && !page.playMode
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 12
            width: 260
            height: 44
            radius: Theme.radius
            color: "#c0111317"
            Rectangle {
                x: 10; y: 8
                width: parent.width - 20
                height: 10
                radius: 2
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: "#30123b" }
                    GradientStop { position: 0.14; color: "#466be3" }
                    GradientStop { position: 0.29; color: "#28bbec" }
                    GradientStop { position: 0.43; color: "#45f87d" }
                    GradientStop { position: 0.57; color: "#baf536" }
                    GradientStop { position: 0.71; color: "#fabb36" }
                    GradientStop { position: 0.86; color: "#ed5c1a" }
                    GradientStop { position: 1.0; color: "#a31202" }
                }
            }
            Label { x: 10; y: 22; text: Theme.fmt(page.c.toneLo, 0) + " Hz"; color: Theme.text2; font.pixelSize: Theme.fsSmall }
            Label { anchors.right: parent.right; anchors.rightMargin: 10; y: 22; text: Theme.fmt(page.c.toneHi, 0) + " Hz"; color: Theme.text2; font.pixelSize: Theme.fsSmall }
            Label { anchors.horizontalCenter: parent.horizontalCenter; y: 22; text: ana.state.mapPixels ? ana.state.mapPixels + " px" : ""; color: Theme.text3; font.pixelSize: Theme.fsSmall }
        }
        Column {
            anchors.centerIn: parent
            visible: !ana.clip.loaded
            spacing: 8
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: ana.state.loading ? "Loading " + (ana.clip.name || "") + "…" : "No recording open"
                color: Theme.text3
                font.pixelSize: 18
            }
            Label {
                visible: !ana.state.loading
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Double-click a recording in the Library (1)."
                color: Theme.text3
            }
            ProgressBar { visible: ana.state.loading === true; width: 260; value: ana.state.progress || 0; anchors.horizontalCenter: parent.horizontalCenter }
        }
    }

    // ---- transport + timeline: one slim strip under the viewer (no panel of its own) ----------------------------------
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: (page.playMode ? pbar.implicitHeight : 0) + 72
        color: Theme.panel
        radius: Theme.radius
        ColumnLayout {
            anchors.fill: parent
            spacing: 0
            PlayerBar {
                id: pbar
                visible: page.playMode
                Layout.fillWidth: true
                onSpeedToolChanged: { page.spA = null; page.spB = null }
            }
            Rectangle { visible: page.playMode; Layout.fillWidth: true; height: 1; color: Theme.bg }
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Plot {
                    anchors.fill: parent
                    anchors.topMargin: 2
                    source: ana
                    kind: "ana:rate"
                    cursorX: page.playMode ? player.t : NaN
                    onMarkDragged: (id, v) => ana.markDrag(id, v)
                    onClicked: (x, y) => { if (page.playMode) player.seek(x) }
                }
                HelpBubble {
                    x: 6; y: 4
                    help: page.playMode ? "Timeline (event rate). Click to move the playhead (white). Pink = window: playback loops inside it (or the whole clip)."
                                        : "Timeline: event rate (max per bin). Amber = onsets, cyan dashed = trigger edges, pink = analysis window (drag the handles)."
                }
            }
        }
    }

    // ---- plots / results: one full-width dock, the view picked by tabs in its header -------------------------------------
    Panel {
        id: bottom
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredHeight: 3
        readonly property bool results: Theme.folded("ana/resultsTab", false)
        readonly property string base: page.c.plot === "pspec" ? "spectrum" : page.c.plot === "psig" ? "signal" : (page.c.plot || "spectrum")
        readonly property bool atHead: page.playMode && (page.c.plot === "pspec" || page.c.plot === "psig")
        readonly property string selName: (ana.rois.find(r => r.id === ana.selRoi) || ana.rois[0] || {name: "–"}).name
        title: results ? "Results"
             : atHead ? (base === "signal" ? "Signal at the playhead" : "Spectrum at the playhead")
             : base === "signal" ? "Signal" : base === "sg" ? "Spectrogram of " + selName : base === "rpm" ? "Speed of " + selName : "Spectrum"
        info: results ? (ana.state.note || "") : ""
        subtitle: results ? "Green = tone above the SNR limit. Click a row to select the ROI."
                : atHead ? "Rolling " + Math.round(player.cfg.plotWin * 1000) + " ms before the playhead, updates while playing"
                : base === "sg" ? "Whole clip, dB (50 dB range); white dots = strongest bin in the tone band; pink = window"
                : base === "rpm" ? "RPM over the clip in " + Theme.fmt(page.c.rpmWin, 2) + " s frames (75 % overlap); dashed = the speed over the analysis window; pink = window"
                : base === "signal" ? "The analysed signal in the window" : "Dashed = tone band; label = picked tone"
        tools: [
            CheckBox {
                visible: page.playMode && !bottom.results && bottom.base !== "sg" && bottom.base !== "rpm"
                text: "At playhead"
                checked: bottom.atHead
                implicitHeight: 22
                font.pixelSize: Theme.fsSmall
                onToggled: ana.setCfg("plot", checked ? (bottom.base === "signal" ? "psig" : "pspec") : bottom.base)
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: "Follow the playhead (rolling window) instead of the analysis window"
            },
            Segmented {
                visible: !bottom.results && bottom.base !== "sg" && bottom.base !== "rpm"
                model: ["Selected ROI", "All ROIs"]
                currentIndex: page.c.compare === "all" ? 1 : 0
                onActivated: (i) => ana.setCfg("compare", i === 1 ? "all" : "selected")
                implicitHeight: 22
                fontSize: Theme.fsSmall
                itemPadding: 8
            },
            Segmented {
                model: ["Spectrum", "Signal", "Spectrogram", "RPM", "Results"]
                readonly property var keys: ["spectrum", "signal", "sg", "rpm", "results"]
                currentIndex: bottom.results ? 4 : Math.max(0, keys.indexOf(bottom.base))
                implicitHeight: 22
                fontSize: Theme.fsSmall
                itemPadding: 8
                onActivated: (i) => {
                    Theme.setFolded("ana/resultsTab", i === 4)
                    if (i === 4) return
                    const k = keys[i]
                    ana.setCfg("plot", bottom.atHead && k !== "sg" && k !== "rpm" ? (k === "signal" ? "psig" : "pspec") : k)
                }
            }
        ]
        Plot {
            visible: !bottom.results
            anchors.fill: parent
            source: bottom.atHead ? player : ana
            kind: bottom.atHead ? (bottom.base === "signal" ? "play:signal" : "play:spectrum")
                : bottom.base === "signal" ? "ana:signal" : bottom.base === "sg" ? "ana:sg" : bottom.base === "rpm" ? "ana:rpm" : "ana:spectrum"
            cursorX: page.playMode && (bottom.base === "sg" || bottom.base === "rpm") ? player.t : NaN
        }
        DataTable {
            visible: bottom.results
            anchors.fill: parent
            model: ana.table
            onRowClicked: (key) => ana.selectRoi(key)
        }
    }
}
