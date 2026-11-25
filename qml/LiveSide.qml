import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

PageScroll {
    id: page
    function mev(v) { return v === undefined || isNaN(v) ? "–" : (v / 1e6).toFixed(2) }
    readonly property var toneMap: {
        const m = {}
        for (const t of app.liveTones) if (t.f > 0) m[t.id] = {text: t.f.toFixed(1) + " Hz · " + t.snr.toFixed(0), ok: t.ok}
        return m
    }

    // ---- camera -----------------------------------------------------------------------------------------------------
    Card {
        title: "Camera"
        glyph: Theme.gCamera
        Segmented {
            visible: edition === "dev"   // release editions have no simulator
            fill: true
            width: parent.width
            model: ["Triton2 EVS", "Simulator"]
            currentIndex: app.st.source === "camera" ? 0 : 1
            onActivated: (i) => app.setv("source", i === 0 ? "camera" : "sim")
        }
        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fsSmall
            color: app.live.connected ? Theme.text2 : Theme.err
            text: app.live.connected ? app.live.source : (app.live.error || "Not connected")
        }
        RowLayout {
            width: parent.width
            spacing: 18
            Stat { caption: "events"; value: page.mev(app.live.rate); unit: "Mev/s" }
            Stat { caption: "link"; value: Theme.fmt(app.live.linkPct, 0); unit: "%"; valueColor: app.live.linkPct > 80 ? Theme.err : app.live.linkPct > 50 ? Theme.warn : Theme.text }
            Stat { caption: "lost"; value: app.live.lost === undefined || isNaN(app.live.lost) ? "0" : String(app.live.lost); valueColor: app.live.lost > 0 ? Theme.err : Theme.text }
        }
        RowLayout {
            width: parent.width
            spacing: 6
            Button { Layout.fillWidth: true; text: app.live.running ? "Stop stream" : "Start stream"; onClicked: app.toggleRun() }
            Button { Layout.fillWidth: true; text: "Reconnect"; onClicked: app.connectSource(app.st.source) }
        }
        Hint {
            visible: app.st.erc_enable !== true && app.live.linkPct > 70
            color: Theme.warn
            text: "The link is above 70 %. Crop the sensor or raise the thresholds: the rate limiter is off so that no event of a recording is dropped."
        }
    }

    // ---- recording ---------------------------------------------------------------------------------------------------
    Card {
        title: "Recording"
        glyph: Theme.gRecord
        subtitle: "Space = record now · A = arm (starts on an event-rate jump) · Esc = stop"
        FormRow {
            label: "Project"
            help: "The library folder the recording goes to. Make new projects on the Library page."
            ComboBox {
                model: lib.projects.map(p => p.label)
                currentIndex: Math.max(0, lib.projects.findIndex(p => p.name === app.st.project))
                onActivated: (i) => app.setv("project", lib.projects[i].name)
            }
        }
        FormRow {
            label: "Name"
            help: "File name start; date and time are added. Letters, digits, - and _."
            TextField { text: app.st.recName; onEditingFinished: app.setv("recName", text); onAccepted: win.releaseFocus() }
        }
        FormRow {
            label: "Description"
            help: "Stored with the recording and shown in the Library (editable there later)."
            TextField { placeholderText: "what, how excited, setup"; text: app.st.recDesc; onEditingFinished: app.setv("recDesc", text); onAccepted: win.releaseFocus() }
        }
        NumberField { label: "Length"; value: app.st.recLen; unit: "s"; decimals: 2; step: 0.1; from: 0.05; to: 120; onEdited: (v) => app.setv("recLen", v); onAccepted: win.releaseFocus(); help: "Time after the trigger. Longer = finer frequency resolution (0.5 Hz at 2 s)." }
        NumberField { label: "Pre-trigger"; value: app.st.preMs; unit: "ms"; decimals: 0; step: 10; from: 0; to: 900; onEdited: (v) => app.setv("preMs", v); onAccepted: win.releaseFocus(); help: "Events before the trigger that go into the file (kept in memory all the time)." }
        NumberField { label: "Arm trigger"; value: app.st.trigFactor; unit: "× rate"; decimals: 1; step: 0.5; from: 1.5; to: 50; onEdited: (v) => app.setv("trigFactor", v); onAccepted: win.releaseFocus(); help: "Armed (A): the recording starts when the event rate in the crop (or in the ROIs, else the whole sensor) over 3 ms jumps above this factor times the mean of the half second before." }
        RowLayout {
            width: parent.width
            spacing: 6
            Button { Layout.fillWidth: true; text: "Record now"; highlighted: true; enabled: app.live.recState === 0 || app.live.recState === 1; onClicked: app.recordNow() }
            Button { Layout.fillWidth: true; text: app.live.recState === 1 ? "Disarm" : "Arm"; enabled: app.live.recState <= 1; onClicked: app.toggleArm() }
        }
        RowLayout {
            visible: app.lastRecording !== ""
            width: parent.width
            Label { Layout.fillWidth: true; text: "Last: " + app.lastRecording.replace(/^.*[\\/]/, ""); color: Theme.text2; font.pixelSize: Theme.fsSmall; elide: Text.ElideMiddle }
            Button { text: "Analyse"; flat: true; onClicked: win.openClip(app.lastRecording) }
            Button { text: "Library"; flat: true; onClicked: { lib.setProject(app.st.project); lib.select(app.lastRecording); app.setv("page", "library") } }
        }
    }

    // ---- ROIs ----------------------------------------------------------------------------------------------------------------
    Card {
        title: "Live ROIs"
        glyph: Theme.gPin
        subtitle: "Draw on the view (L line, R rectangle, G polygon, B brush). Each ROI gets a rolling spectrum; the tone and its SNR show here."
        RoiList { ctl: app; tones: page.toneMap; showSnap: true }
        RowLayout {
            width: parent.width
            spacing: 6
            ComboBox {
                id: recipeBox
                Layout.fillWidth: true
                property var items: []
                function reload() { items = lib.recipes(); model = items.map(r => r.label) }
                Component.onCompleted: reload()
                displayText: items.length ? "From: " + currentText : "no saved measurements in " + (lib.project || "Unsorted")
                onActivated: (i) => app.loadRoisFrom(items[i].dir)
                onPressedChanged: if (pressed) reload()
            }
            Button { text: "Clear"; enabled: app.rois.length > 0; onClicked: app.clearRois() }
        }
        Hint { help: true; text: "\"From\" takes the ROIs, signal and window settings of a measurement saved on the Analyse page (Library project " + (lib.project || "Unsorted") + ")." }
    }

    // ---- rotation speed ----------------------------------------------------------------------------------------------------------
    Card {
        title: "Rotation speed"
        glyph: Theme.gLoop
        info: app.st.rpmOn ? (app.live.rpmOk === true ? app.live.rpm.toFixed(1) + " rpm" : "no speed") : "off"
        subtitle: "Draw a small ROI where the blades / teeth / a tape mark pass by (not over the hub). The selected ROI (else the first) gives the speed."
        SwitchRow { label: "Measure RPM"; key: "rpmOn" }
        RpmSettingsForm { cfg: app.st; set: (k, v) => app.setv(k, v); live: true }
    }

    // ---- analysis -------------------------------------------------------------------------------------------------------------
    Card {
        title: "Live spectrum"
        glyph: Theme.gWave
        FormRow {
            label: "Signal"
            help: "Velocity: the ON−OFF rate itself (no filter, best for short windows). Displacement: integrated, detrended and band-passed."
            Segmented { fill: true; model: ["Velocity", "Displacement"]; currentIndex: app.st.liveSignal === "displacement" ? 1 : 0; onActivated: (i) => app.setv("liveSignal", i === 1 ? "displacement" : "velocity") }
        }
        RowLayout {
            width: parent.width
            spacing: 6
            NumberField { Layout.fillWidth: true; label: "Tone"; labelWidth: 0.4; value: app.st.toneLo; unit: "Hz"; decimals: 0; step: 10; from: 1; to: 4900; onEdited: (v) => app.setv("toneLo", v); onAccepted: win.releaseFocus() }
            NumberField { Layout.fillWidth: true; label: "to"; labelWidth: 0.25; value: app.st.toneHi; unit: "Hz"; decimals: 0; step: 10; from: 2; to: 4990; onEdited: (v) => app.setv("toneHi", v); onAccepted: win.releaseFocus() }
        }
        RowLayout {
            width: parent.width
            spacing: 6
            NumberField { Layout.fillWidth: true; label: "Band"; labelWidth: 0.4; value: app.st.bandLo; unit: "Hz"; decimals: 0; step: 10; from: 1; to: 4900; onEdited: (v) => app.setv("bandLo", v); onAccepted: win.releaseFocus(); help: "Band-pass of the displacement signal and the reference band of the SNR (peak / median of the band)." }
            NumberField { Layout.fillWidth: true; label: "to"; labelWidth: 0.25; value: app.st.bandHi; unit: "Hz"; decimals: 0; step: 10; from: 2; to: 4990; onEdited: (v) => app.setv("bandHi", v); onAccepted: win.releaseFocus() }
        }
        NumberField { label: "Window"; value: app.st.liveWin; unit: "s"; decimals: 2; step: 0.1; from: 0.05; to: 5; onEdited: (v) => app.setv("liveWin", v); onAccepted: win.releaseFocus(); help: "Length of the rolling spectrum." }
        NumberField { label: "SNR limit"; value: app.st.snrMin; unit: "×"; decimals: 0; step: 1; from: 1; to: 1000; onEdited: (v) => app.setv("snrMin", v); onAccepted: win.releaseFocus() }
        RowLayout {
            width: parent.width
            spacing: 6
            NumberField { Layout.fillWidth: true; label: "Plot"; labelWidth: 0.4; value: app.st.specMin; unit: "Hz"; decimals: 0; step: 10; from: 0; to: 4990; onEdited: (v) => app.setv("specMin", Math.min(v, app.st.specMax - 10)); onAccepted: win.releaseFocus(); help: "Frequency range of the live spectrum plot. The y scale follows the peaks inside this range only." }
            NumberField { Layout.fillWidth: true; label: "to"; labelWidth: 0.25; value: app.st.specMax; unit: "Hz"; decimals: 0; step: 100; from: 20; to: 5000; onEdited: (v) => app.setv("specMax", Math.max(v, app.st.specMin + 10)); onAccepted: win.releaseFocus() }
        }
        RowLayout {
            width: parent.width
            spacing: 6
            Button { Layout.fillWidth: true; flat: true; text: "Plot = band"; onClicked: { app.setv("specMin", app.st.bandLo); app.setv("specMax", app.st.bandHi) } }
            Button { Layout.fillWidth: true; flat: true; text: "Plot = tone range"; onClicked: { app.setv("specMin", app.st.toneLo); app.setv("specMax", app.st.toneHi) } }
            Button { Layout.fillWidth: true; flat: true; text: "0 – 1200 Hz"; onClicked: { app.setv("specMin", 0); app.setv("specMax", 1200) } }
        }
    }

    // ---- auto-measure ------------------------------------------------------------------------------------------------------------
    Card {
        title: "Auto-measure"
        glyph: Theme.gBolt
        subtitle: "On every pluck / tap: take a short window after it, measure every ROI, log the tone. For stepped tests: step, pluck, read, step."
        SwitchRow { label: "Measure on every trigger"; key: "amOn" }
        FormRow {
            label: "Trigger"
            help: "Event-rate jump: the 3 ms rate in the crop / ROIs jumps above the factor times the half second before. External: rising edge on the camera's trigger input (EXT_TRIGGER events in the stream)."
            Segmented { fill: true; model: ["Rate jump", "External"]; currentIndex: app.st.amSource; onActivated: (i) => app.setv("amSource", i) }
        }
        SwitchRow { visible: app.st.amSource === 0; label: "Only the crop / ROIs count"; key: "amRegion"; help: "Off: the whole sensor's event rate must jump. On: only events in the sensor crop (or, without a crop, in the ROIs), so other motion in the picture does not trigger." }
        NumberField { visible: app.st.amSource === 0; label: "Jump factor"; value: app.st.amFactor; unit: "×"; decimals: 1; step: 0.5; from: 1.5; to: 50; onEdited: (v) => app.setv("amFactor", v); onAccepted: win.releaseFocus() }
        NumberField { label: "Min. gap"; value: app.st.amGap; unit: "s"; decimals: 2; step: 0.05; from: 0.02; to: 10; onEdited: (v) => app.setv("amGap", v); onAccepted: win.releaseFocus(); help: "A new trigger within this time after the last one is ignored (bounces, precursors)." }
        RowLayout {
            width: parent.width
            spacing: 6
            NumberField { Layout.fillWidth: true; label: "Skip"; labelWidth: 0.4; value: app.st.amSkipMs; unit: "ms"; decimals: 0; step: 1; from: 0; to: 1000; onEdited: (v) => app.setv("amSkipMs", v); onAccepted: win.releaseFocus(); help: "Time after the trigger that is skipped (the broadband hit itself). 8 ms worked for plucks." }
            NumberField { Layout.fillWidth: true; label: "Window"; labelWidth: 0.45; value: app.st.amLenMs; unit: "ms"; decimals: 0; step: 5; from: 5; to: 5000; onEdited: (v) => app.setv("amLenMs", v); onAccepted: win.releaseFocus(); help: "50 ms: within ±0.75 % of the long-window tone for a hard pluck; 30 ms only when the onset is exact." }
        }
        FormRow {
            label: "Method"
            help: "Rect + velocity is the most precise for short windows (zero-padded to 0.25 Hz bins)."
            Segmented {
                fill: true
                model: ["Velocity · rect", "Velocity · Hann", "Displ. · Hann"]
                currentIndex: app.st.amSignal === "displacement" ? 2 : app.st.amWindow === "hann" ? 1 : 0
                onActivated: (i) => { app.setv("amSignal", i === 2 ? "displacement" : "velocity"); app.setv("amWindow", i === 0 ? "rect" : "hann") }
            }
        }
        NumberField { label: "SNR limit"; value: app.st.amSnr; unit: "×"; decimals: 0; step: 1; from: 1; to: 1000; onEdited: (v) => app.setv("amSnr", v); onAccepted: win.releaseFocus() }
        SwitchRow { label: "Also save a clip of each trigger"; key: "amSave" }
        NumberField { visible: app.st.amSave === true; label: "Clip length"; value: app.st.amSaveLen; unit: "s"; decimals: 2; step: 0.1; from: 0.05; to: 10; onEdited: (v) => app.setv("amSaveLen", v); onAccepted: win.releaseFocus() }
        RowLayout {
            width: parent.width
            Label { Layout.fillWidth: true; text: app.amCount + " measured" + (app.live.amPending > 0 ? ", " + app.live.amPending + " waiting" : ""); color: Theme.text2; font.pixelSize: Theme.fsSmall }
            Button { text: "Clear log"; flat: true; enabled: app.amCount > 0; onClicked: app.clearAmLog() }
        }
        Hint { help: true; text: "Each result is appended to automeasure_<date>.csv in the project folder (one line per ROI)." }
    }

    // ---- setup aids -------------------------------------------------------------------------------------------------------------
    Card {
        title: "Setup aids"
        glyph: Theme.gView
        Label { text: "Web view"; color: Theme.text; font.weight: Font.DemiBold }
        SwitchRow { label: "Live view in a browser"; key: "webOn"; help: "Read-only: the live picture with the ROIs, their tones and the live spectra, for any browser on the same network. Nothing can be changed from the browser." }
        NumberField { visible: app.st.webOn === true; label: "Port"; value: app.st.webPort; decimals: 0; step: 1; from: 1024; to: 65535; onEdited: (v) => app.setv("webPort", Math.round(v)); onAccepted: win.releaseFocus() }
        Repeater {
            model: app.st.webOn === true ? (app.live.webUrls || []) : []
            TextEdit {
                required property string modelData
                text: modelData
                readOnly: true
                selectByMouse: true
                color: Theme.accent
                font.family: Theme.mono
                font.pixelSize: Theme.fsSmall
            }
        }
        Hint {
            visible: app.st.webOn === true
            text: app.live.webError ? app.live.webError : (app.live.webViewers > 0 ? app.live.webViewers + " watching" : "Nobody watching (costs nothing then)")
        }
        Label { text: "Focus chart"; color: Theme.text; font.weight: Font.DemiBold }
        Hint { help: true; text: "A blinking Siemens star for a screen in front of the camera. Set the view to Events with a short fade, zoom into a star centre and turn the focus ring until the blurred disc in the middle is smallest. The corner stars show focus over the field." }
        NumberField { label: "Rays"; value: app.st.chartRays; decimals: 0; step: 4; from: 8; to: 144; onEdited: (v) => app.setv("chartRays", Math.round(v)); onAccepted: win.releaseFocus(); help: "More rays = finer detail near the centre (use more for a large screen or a long distance)." }
        NumberField { label: "Blink"; value: app.st.chartHz; unit: "Hz"; decimals: 1; step: 0.5; from: 0.5; to: 30; onEdited: (v) => app.setv("chartHz", v); onAccepted: win.releaseFocus(); help: "Cycles per second. A few Hz is enough; keep it well below the screen refresh rate." }
        FormRow {
            label: "Blink to"
            help: "Negative: chart and its negative alternate, every edge fires on each flip (most events). Black: chart on / off."
            Segmented { fill: true; model: ["Negative", "Black"]; currentIndex: app.st.chartInvert === false ? 1 : 0; onActivated: (i) => app.setv("chartInvert", i === 0) }
        }
        FormRow {
            visible: Qt.application.screens.length > 1
            label: "Screen"
            ComboBox {
                id: chartScreen
                Layout.fillWidth: true
                model: Qt.application.screens.map((s, i) => (i + 1) + ": " + s.name + " (" + s.width + "×" + s.height + ")")
                currentIndex: Qt.application.screens.length > 1 ? 1 : 0
            }
        }
        RowLayout {
            width: parent.width
            spacing: 6
            Button { Layout.fillWidth: true; text: focusChart.visible ? "Chart: full screen" : "Open chart full screen"; onClicked: focusChart.showOn(chartScreen.currentIndex, true) }
            Button { Layout.fillWidth: true; text: focusChart.visible ? "Close chart" : "Open in a window"; onClicked: focusChart.visible ? focusChart.close() : focusChart.showOn(chartScreen.currentIndex, false) }
        }
        Label { text: "Reference picture"; color: Theme.text; font.weight: Font.DemiBold }
        Hint { help: true; text: "Store the current picture and lay it over the live view to put the camera back in the same place another day." }
        RowLayout {
            width: parent.width
            spacing: 6
            Button { Layout.fillWidth: true; text: "Capture now"; onClicked: app.captureReference() }
            Button { Layout.fillWidth: true; text: "Clear"; enabled: app.st.refOn === true; onClicked: app.clearReference() }
        }
        SwitchRow { label: "Show the reference"; key: "refOn" }
        BiasSlider { visible: app.st.refOn === true; label: "Opacity"; key: "refOpacity"; from: 0.05; to: 1; step: 0.05; decimals: 2 }
        Label { text: "Noise floor"; color: Theme.text; font.weight: Font.DemiBold }
        Button {
            width: parent.width
            text: app.live.autoText && app.live.autoText.indexOf("Looking") === 0 ? "Stop (restores the biases)" : "Find the noise floor (scene must be still)"
            enabled: app.live.running === true
            onClicked: app.autoSensitivity()
        }
        BiasSlider { label: "Noise limit"; key: "autoNoiseKev"; from: 1; to: 300; unit: "kev/s"; help: "What counts as noise for the finder: the event rate of a still scene. Lower = stricter = less sensitive result." }
        Hint { visible: !!app.live.autoText; text: app.live.autoText || "" }
    }

    // ---- biases ----------------------------------------------------------------------------------------------------------
    Card {
        title: "Sensitivity"
        glyph: Theme.gTarget
        subtitle: "IMX636 bias offsets (0 = factory). Lower thresholds = more sensitive and more noise."
        RowLayout {
            width: parent.width
            spacing: 6
            ComboBox {
                Layout.fillWidth: true
                model: app.presets
                displayText: app.st.preset === "custom" ? "Custom (unsaved)" : app.st.preset
                currentIndex: app.presets.indexOf(app.st.preset)
                onActivated: (i) => app.applyPreset(app.presets[i])
            }
            Button { text: "Save as…"; onClicked: presetPop.open() }
            IconButton { glyph: Theme.gDelete; size: 30; tip: "Delete this preset"; enabled: app.st.preset !== "custom" && app.st.preset !== "Factory"; onClicked: app.deletePreset(app.st.preset) }
        }
        BiasSlider { label: "ON threshold"; key: "bias_diff_on"; from: -88; to: 152; help: "bias_diff_on: contrast step for an ON (brighter) event." }
        BiasSlider { label: "OFF threshold"; key: "bias_diff_off"; from: -38; to: 202; help: "bias_diff_off: contrast step for an OFF (darker) event." }
        BiasSlider { label: "Refractory"; key: "bias_refr"; from: -20; to: 235; help: "bias_refr: dead time of a pixel after an event. Higher = shorter dead time. A vibrating edge pixel fires every half period: keep the dead time well below it." }
        BiasSlider { label: "High-pass"; key: "bias_hpf"; from: 0; to: 120; help: "bias_hpf: pixel high-pass. Removes slow changes (rocking, drift) before they become events." }
        BiasSlider { label: "Low-pass"; key: "bias_fo"; from: -43; to: 22; help: "bias_fo: pixel bandwidth. Lower = less high-frequency noise; keep it well above the tones of interest." }
        SwitchRow { label: "Anti-flicker band-stop"; key: "afk_enable"; help: "IMX636 anti-flicker (50–500 Hz only), used as a band-STOP: periodic activity in the band is dropped in the sensor." }
        RowLayout {
            width: parent.width
            visible: app.st.afk_enable === true
            spacing: 6
            NumberField { Layout.fillWidth: true; label: "from"; labelWidth: 0.35; value: app.st.afk_low; unit: "Hz"; decimals: 0; step: 1; from: 50; to: 499; onEdited: (v) => app.setv("afk_low", v); onAccepted: win.releaseFocus() }
            NumberField { Layout.fillWidth: true; label: "to"; labelWidth: 0.25; value: app.st.afk_high; unit: "Hz"; decimals: 0; step: 1; from: 51; to: 500; onEdited: (v) => app.setv("afk_high", v); onAccepted: win.releaseFocus() }
        }
        SwitchRow { label: "Trail filter"; key: "trail_enable"; help: "Removes repeated events of a pixel: it eats the events of a vibrating edge. Keep off for vibration." }
        SwitchRow { label: "Event rate control (ERC)"; key: "erc_enable"; help: "Caps the event rate by DROPPING events. Off for measurements; only on if the link overflows." }
        NumberField { visible: app.st.erc_enable === true; label: "ERC limit"; value: app.st.erc_rate; unit: "Mev/s"; decimals: 0; step: 5; from: 1; to: 200; onEdited: (v) => app.setv("erc_rate", v); onAccepted: win.releaseFocus() }
    }

    // ---- sensor crop -------------------------------------------------------------------------------------------------------------
    Card {
        title: "Sensor crop"
        glyph: Theme.gGrid
        subtitle: "Crop tool (toolbar): drag a rectangle. In the camera it drops every event outside it (less data, no link overflow); it is also the region of the triggers."
        SwitchRow { label: "Crop on"; key: "roiOn" }
        SwitchRow { label: "Apply in the camera (hardware)"; key: "roiHw" }
        Label {
            width: parent.width
            font.family: Theme.mono
            font.pixelSize: Theme.fsSmall
            color: Theme.text2
            text: "x " + Theme.fmt(app.st.roiX, 0) + "  y " + Theme.fmt(app.st.roiY, 0) + "  " + Theme.fmt(app.st.roiW, 0) + " × " + Theme.fmt(app.st.roiH, 0) + " px"
        }
    }

    // ---- simulator ------------------------------------------------------------------------------------------------------------
    Card {
        visible: app.st.source === "sim" && edition === "dev"
        title: "Simulator"
        glyph: Theme.gBench
        subtitle: "A lab bench: four dark bars ring at 212.4, 287.65, 335.2 and 401.85 Hz after every tap (with slow rocking), and a fan on the right turns at the set speed (±0.5 % wander) with a bright tape mark on one blade tip."
        Button { width: parent.width; text: "Tap  [T]"; onClicked: app.simTap() }
        NumberField { label: "Tap every"; value: app.st.simTapEvery; unit: "s"; decimals: 1; step: 0.5; from: 0; to: 60; onEdited: (v) => app.setv("simTapEvery", v); onAccepted: win.releaseFocus(); help: "0 = only when you press Tap." }
        NumberField { label: "Background noise"; value: app.st.simNoise; unit: "Hz/px"; decimals: 2; step: 0.05; from: 0; to: 20; onEdited: (v) => app.setv("simNoise", v); onAccepted: win.releaseFocus() }
        NumberField { label: "Rocking"; value: app.st.simRock; unit: "× tone"; decimals: 1; step: 0.5; from: 0; to: 20; onEdited: (v) => app.setv("simRock", v); onAccepted: win.releaseFocus() }
        SwitchRow { label: "Fan"; key: "simFan" }
        NumberField { visible: app.st.simFan === true; label: "Fan speed"; value: app.st.simFanRpm; unit: "rpm"; decimals: 0; step: 50; from: 0; to: 6000; onEdited: (v) => app.setv("simFanRpm", v); onAccepted: win.releaseFocus(); help: "Above ~1500 rpm the simulator falls behind real time (many events)." }
        NumberField { visible: app.st.simFan === true; label: "Blades"; value: app.st.simBlades; decimals: 0; step: 1; from: 1; to: 12; onEdited: (v) => app.setv("simBlades", Math.round(v)); onAccepted: win.releaseFocus() }
        SwitchRow { visible: app.st.simFan === true; label: "Tape mark on blade 1"; key: "simTape" }
    }

    Popup {
        id: presetPop
        anchors.centerIn: Overlay.overlay
        modal: true
        padding: 16
        background: Rectangle { color: Theme.card; radius: Theme.radius; border.color: Theme.borderStrong }
        onOpened: { presetName.text = ""; presetName.forceActiveFocus() }
        Column {
            spacing: 10
            width: 300
            Label { text: "Save the camera settings as a preset"; color: Theme.text; font.weight: Font.DemiBold }
            TextField { id: presetName; width: parent.width; placeholderText: "preset name"; onAccepted: { app.savePreset(text); presetPop.close() } }
            RowLayout {
                width: parent.width
                Button { Layout.fillWidth: true; text: "Save"; highlighted: true; onClicked: presetName.accepted() }
                Button { Layout.fillWidth: true; text: "Cancel"; onClicked: presetPop.close() }
            }
        }
    }
}
