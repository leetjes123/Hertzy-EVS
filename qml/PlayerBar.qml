import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Transport + rendering settings of the clip player (Analyse > Play).
Rectangle {
    id: bar
    readonly property var c: player.cfg
    property bool speedTool: false
    implicitHeight: col.implicitHeight + 10
    color: "transparent"

    readonly property var speeds: [
        {t: "2×", v: 2}, {t: "1×", v: 1}, {t: "1/2", v: 0.5}, {t: "1/5", v: 0.2}, {t: "1/10", v: 0.1}, {t: "1/20", v: 0.05},
        {t: "1/50", v: 0.02}, {t: "1/100", v: 0.01}, {t: "1/200", v: 0.005}, {t: "1/500", v: 0.002}, {t: "1/1000", v: 0.001},
        {t: "1/2000", v: 0.0005}, {t: "1/5000", v: 0.0002}, {t: "1/10 000", v: 0.0001}, {t: "1/100 000", v: 0.00001}]
    readonly property var exposures: [10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000]
    readonly property var trails: [0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000]
    function nearest(list, v, key) {
        let b = 0
        for (let i = 0; i < list.length; ++i) {
            const x = key ? list[i][key] : list[i]
            const y = key ? list[b][key] : list[b]
            if (Math.abs(Math.log(x / v)) < Math.abs(Math.log(y / v))) b = i
        }
        return b
    }
    function us(v) { return v >= 1000 ? (v / 1000) + " ms" : v + " µs" }
    function clock(s) { return s < 1 ? (s * 1000).toFixed(3) + " ms" : s.toFixed(5) + " s" }

    ColumnLayout {
        id: col
        x: 8; y: 5
        width: parent.width - 16
        spacing: 4
        // ---- transport
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            IconButton { glyph: Theme.gRestart; tip: "To the start of the range [Home]"; onClicked: player.home() }
            IconButton { glyph: Theme.gChevronLeft; tip: "One step back [←] (strobe: one period)"; onClicked: player.step(-1) }
            Button {
                text: player.playing ? "❚❚  Pause" : "▶  Play"
                highlighted: true
                implicitWidth: 110
                enabled: player.loaded
                onClicked: player.toggle()
                ToolTip.text: "Space"
                ToolTip.visible: hovered
                ToolTip.delay: 600
            }
            IconButton { glyph: Theme.gChevronRight; tip: "One step forward [→]"; onClicked: player.step(1) }
            Label {
                text: "t " + bar.clock(player.t)
                font.family: Theme.mono
                color: Theme.text
                Layout.preferredWidth: 150
            }
            Segmented {
                model: ["Window", "Whole clip"]
                currentIndex: bar.c.loop === "clip" ? 1 : 0
                onActivated: (i) => player.setv("loop", i === 1 ? "clip" : "window")
                fontSize: Theme.fsSmall
                itemPadding: 8
            }
            Item { Layout.fillWidth: true }
            Button {
                text: bar.speedTool ? "Speed tool: click A, then B" : "Measure speed"
                checkable: true
                checked: bar.speedTool
                onToggled: bar.speedTool = checked
                ToolTip.text: "Click where a moving object was, then where it is later: speed = distance / time between the two events. Works best in Trail mode."
                ToolTip.visible: hovered
                ToolTip.delay: 600
            }
            Button {
                text: player.exportState.busy ? "Cancel " + Math.round(player.exportState.progress * 100) + " %" : "Export MP4"
                enabled: player.loaded
                onClicked: player.exportState.busy ? player.cancelExport() : player.exportVideo(30)
                ToolTip.text: "Renders the range (window or whole clip) with these settings at 30 fps into <clip>.video/ next to the recording. Needs ffmpeg (else PNG frames)."
                ToolTip.visible: hovered
                ToolTip.delay: 600
            }
        }
        // ---- rendering
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Segmented {
                model: ["Events", "Trail", "Heat"]
                readonly property var keys: ["events", "trail", "heat"]
                currentIndex: Math.max(0, keys.indexOf(bar.c.mode))
                onActivated: (i) => player.setv("mode", keys[i])
                fontSize: Theme.fsSmall
                itemPadding: 8
            }
            Switch {
                visible: bar.c.mode !== "trail"
                text: "Bold"
                checked: bar.c.bold !== false
                onToggled: player.setv("bold", checked)
                ToolTip.text: "Every event as 2 × 2 pixels: stays visible when the picture is scaled down (presentations, videos)"
                ToolTip.visible: hovered
                ToolTip.delay: 600
            }
            Label { text: "Speed"; color: Theme.text2; visible: !bar.c.strobe }
            ComboBox {
                visible: !bar.c.strobe
                model: bar.speeds.map(s => s.t)
                currentIndex: bar.nearest(bar.speeds, bar.c.speed, "v")
                onActivated: (i) => player.setv("speed", bar.speeds[i].v)
                implicitWidth: 110
            }
            Label { text: bar.c.mode === "trail" ? "Trail" : "Exposure"; color: Theme.text2 }
            ComboBox {
                visible: bar.c.mode !== "trail"
                model: bar.exposures.map(v => bar.us(v))
                currentIndex: bar.nearest(bar.exposures, bar.c.exposureUs)
                onActivated: (i) => player.setv("exposureUs", bar.exposures[i])
                implicitWidth: 100
            }
            ComboBox {
                visible: bar.c.mode === "trail"
                model: bar.trails.map(v => v + " ms")
                currentIndex: bar.nearest(bar.trails, bar.c.trailMs)
                onActivated: (i) => player.setv("trailMs", bar.trails[i])
                implicitWidth: 100
            }
            Rectangle { width: 1; height: 24; color: Theme.border }
            Switch {
                text: "Strobe"
                checked: bar.c.strobe === true
                onToggled: player.setv("strobe", checked)
                ToolTip.text: "Every picture jumps one period: anything that repeats at this frequency (fan blades, a ringing bar) stands still; the drift makes it turn slowly."
                ToolTip.visible: hovered
                ToolTip.delay: 600
            }
            NumberField {
                visible: bar.c.strobe === true
                Layout.preferredWidth: 170
                label: "at"
                labelWidth: 0.2
                value: bar.c.strobeHz
                unit: "Hz"
                decimals: 2
                step: 0.1
                from: 0.01
                to: 100000
                onEdited: (v) => player.setv("strobeHz", v)
                onAccepted: win.releaseFocus()
            }
            NumberField {
                visible: bar.c.strobe === true
                Layout.preferredWidth: 150
                label: "drift"
                labelWidth: 0.35
                value: bar.c.strobeDrift
                unit: "%"
                decimals: 1
                step: 0.5
                from: -50
                to: 50
                onEdited: (v) => player.setv("strobeDrift", v)
                onAccepted: win.releaseFocus()
            }
            Button {
                visible: bar.c.strobe === true
                flat: true
                text: "= ROI tone"
                onClicked: {
                    const sel = ana.results.find(r => r.id === ana.selRoi && r.ok) || ana.results.find(r => r.ok)
                    const pt = player.tones.find(r => r.id === ana.selRoi && r.ok) || player.tones.find(r => r.ok)
                    const f = sel ? sel.f : pt ? pt.f : 0
                    if (f > 0) player.setv("strobeHz", f)
                }
                ToolTip.text: "Strobe at the tone of the selected ROI (for a fan: an ROI on the blades gives blades × revolutions per second)"
                ToolTip.visible: hovered
                ToolTip.delay: 600
            }
            Item { Layout.fillWidth: true }
            Label {
                text: player.exportState.text || ""
                color: Theme.text3
                elide: Text.ElideMiddle
                Layout.maximumWidth: 380
                font.pixelSize: Theme.fsSmall
            }
        }
    }
}
