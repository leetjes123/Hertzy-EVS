import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

PageScroll {
    id: page
    readonly property var c: ana.cfg
    readonly property var toneMap: {
        const m = {}
        for (const r of ana.results) if (r.f > 0) m[r.id] = {text: r.f.toFixed(1) + " Hz · " + r.snr.toFixed(0), ok: r.ok}
        return m
    }
    function num(k, v) { ana.setCfg(k, v) }

    // ---- clip ---------------------------------------------------------------------------------------------------------
    Card {
        title: ana.clip.name || "No recording open"
        glyph: Theme.gChart
        collapsible: false
        info: ana.state.loading ? "Loading… " + Math.round(100 * ana.state.progress) + " %"
                : ana.clip.loaded ? (ana.clip.project + "   ·   " + Theme.fmt(ana.clip.duration, 3) + " s   ·   " + (ana.clip.events / 1e6).toFixed(2) + " M events"
                                     + (ana.clip.triggers > 0 ? "   ·   " + ana.clip.triggers + " trigger edges" : ""))
                : "Double-click a recording in the Library, or open a .raw file."
        ProgressBar { visible: ana.state.loading === true; width: parent.width; value: ana.state.progress || 0 }
        Label { visible: !!ana.clip.description; width: parent.width; wrapMode: Text.WordWrap; text: ana.clip.description || ""; color: Theme.text2; font.italic: true }
        Label { visible: !!ana.state.error; width: parent.width; wrapMode: Text.WordWrap; text: ana.state.error || ""; color: Theme.err }
        RowLayout {
            width: parent.width
            spacing: 6
            Button { Layout.fillWidth: true; text: "Library"; onClicked: { if (ana.clip.path) lib.select(ana.clip.path); app.setv("page", "library") } }
            Button { Layout.fillWidth: true; text: "Open file…"; onClicked: openDlg.open() }
            IconButton { glyph: Theme.gFolder; size: 32; tip: "Show the measurement files"; enabled: !!ana.clip.loaded; onClicked: ana.showFolder() }
        }
    }

    // ---- time window ----------------------------------------------------------------------------------------------------
    Card {
        title: "Time window"
        glyph: Theme.gLoop
        subtitle: "Spectra, tones and (by default) the map use this window. Drag the pink handles on the timeline for a custom window."
        Segmented {
            fill: true
            width: parent.width
            model: ["Whole clip", "From onset", "Custom"]
            currentIndex: page.c.winMode === "onset" ? 1 : (page.c.t1 > page.c.t0 ? 2 : 0)
            onActivated: (i) => {
                if (i === 0) ana.setWindow(0, 0)
                else if (i === 1) ana.useOnset(Math.min(page.c.onset, Math.max(0, ana.onsets.length - 1)))
                else ana.setWindow(ana.state.t0 || 0, (ana.state.t1 || ana.clip.duration || 1))
            }
        }
        Label {
            visible: page.c.winMode === "onset" && ana.onsets.length === 0 && !!ana.clip.loaded
            text: "No onset (tap / pluck / trigger) found in this clip."
            color: Theme.warn
            font.pixelSize: Theme.fsSmall
        }
        FormRow {
            visible: page.c.winMode === "onset" && ana.onsets.length > 0
            label: "Onset"
            help: "Taps / plucks found in the clip: external trigger edges when the file has them, else steep jumps of the event rate (onset = first ms above half the jump)."
            ComboBox {
                model: ana.onsets.map((o, i) => (i + 1) + ":  " + o.t.toFixed(3) + " s  (" + o.source + ")")
                currentIndex: Math.min(page.c.onset, ana.onsets.length - 1)
                onActivated: (i) => ana.useOnset(i)
            }
        }
        RowLayout {
            visible: page.c.winMode === "onset"
            width: parent.width
            spacing: 6
            NumberField { Layout.fillWidth: true; label: "Skip"; labelWidth: 0.4; value: page.c.skip * 1000; unit: "ms"; decimals: 1; step: 1; from: 0; to: 5000; onEdited: (v) => page.num("skip", v / 1000); onAccepted: win.releaseFocus() }
            NumberField { Layout.fillWidth: true; label: "Length"; labelWidth: 0.45; value: page.c.len * 1000; unit: "ms"; decimals: 0; step: 10; from: 5; to: 60000; onEdited: (v) => page.num("len", v / 1000); onAccepted: win.releaseFocus() }
        }
        RowLayout {
            visible: page.c.winMode !== "onset" && page.c.t1 > page.c.t0
            width: parent.width
            spacing: 6
            NumberField { Layout.fillWidth: true; label: "From"; labelWidth: 0.4; value: page.c.t0; unit: "s"; decimals: 3; step: 0.01; from: 0; to: 3600; onEdited: (v) => ana.setWindow(v, page.c.t1); onAccepted: win.releaseFocus() }
            NumberField { Layout.fillWidth: true; label: "to"; labelWidth: 0.3; value: page.c.t1; unit: "s"; decimals: 3; step: 0.01; from: 0; to: 3600; onEdited: (v) => ana.setWindow(page.c.t0, v); onAccepted: win.releaseFocus() }
        }
        Label {
            width: parent.width
            font.family: Theme.mono
            font.pixelSize: Theme.fsSmall
            color: ana.state.winOk ? Theme.text2 : Theme.warn
            text: ana.state.winOk ? "window " + Theme.fmt(ana.state.t0, 4) + " – " + Theme.fmt(ana.state.t1, 4) + " s  (" + Theme.fmt((ana.state.t1 - ana.state.t0) * 1000, 0) + " ms, bins " + Theme.fmt(1 / Math.max(1e-6, ana.state.t1 - ana.state.t0), 2) + " Hz)"
                                  : (ana.state.note || "")
        }
        Label { text: "Quick setups"; color: Theme.text2; font.pixelSize: Theme.fsSmall }
        RowLayout {
            width: parent.width
            spacing: 6
            Button {
                Layout.fillWidth: true
                text: "Ring-down (0.6 s)"
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: "Onset +10 ms, 590 ms, displacement signal, Hann window: the most precise tone of a ringing object."
                onClicked: { ana.setCfg("signal", "displacement"); ana.setCfg("window", "hann"); ana.setCfg("skip", 0.01); ana.setCfg("len", 0.59); ana.useOnset(page.c.onset) }
            }
            Button {
                Layout.fillWidth: true
                text: "Pluck (50 ms)"
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: "Onset +8 ms, 50 ms, velocity (ON−OFF rate) with a rectangular window: fast, within ~0.75 % for hard plucks."
                onClicked: { ana.setCfg("signal", "velocity"); ana.setCfg("window", "rect"); ana.setCfg("skip", 0.008); ana.setCfg("len", 0.05); ana.useOnset(page.c.onset) }
            }
        }
    }

    // ---- map --------------------------------------------------------------------------------------------------------------------
    Card {
        title: "Map"
        glyph: Theme.gPalette
        subtitle: "Settings of the map picked in the toolbar (Activity / ON-OFF / Frequency / Energy). Frequency and energy maps compute a spectrum for every active pixel (up to 1 s of the window)."
        // which map: in the toolbar above the picture
        FormRow {
            label: "Map time"
            Segmented { fill: true; model: ["Window", "Whole clip"]; currentIndex: page.c.mapWin === "all" ? 1 : 0; onActivated: (i) => page.num("mapWin", i === 1 ? "all" : "window") }
        }
        NumberField { visible: page.c.map === "freq"; label: "Show from SNR"; value: page.c.mapSnr; unit: "×"; decimals: 0; step: 1; from: 1; to: 100; onEdited: (v) => page.num("mapSnr", v); onAccepted: win.releaseFocus(); help: "Pixels whose own peak is weaker are drawn dark." }
        NumberField { visible: page.c.map === "freq" || page.c.map === "energy"; label: "Max. pixels"; value: page.c.mapMaxPix; decimals: 0; step: 5000; from: 1000; to: 400000; onEdited: (v) => page.num("mapMaxPix", Math.round(v)); onAccepted: win.releaseFocus(); help: "The most active pixels get a spectrum. More = slower (and more memory)." }
        Hint { visible: page.c.map === "freq"; text: "Colour = strongest frequency of each pixel in the tone band " + Theme.fmt(page.c.toneLo, 0) + "–" + Theme.fmt(page.c.toneHi, 0) + " Hz (blue low, red high), brightness = its amplitude. Hover a pixel to read it. Paint a brush ROI over a region of one colour." }
        Hint { visible: !!ana.state.mapNote; text: ana.state.mapNote || "" }
    }

    // ---- ROIs ---------------------------------------------------------------------------------------------------------------------
    Card {
        title: "ROIs"
        glyph: Theme.gPin
        subtitle: "Draw on the map (L line, R rectangle, G polygon, B brush, E erase; V select / move / reshape) or let the detector place them."
        RoiList { ctl: ana; tones: page.toneMap }
        RowLayout {
            width: parent.width
            spacing: 6
            Button {
                Layout.fillWidth: true
                text: "Find lines"
                enabled: !!ana.clip.loaded
                onClicked: ana.detect("lines", replaceBox.checked)
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: "Straight edges in the activity of the map window (orientation-guided Hough): each becomes a sided line ROI."
            }
            Button {
                Layout.fillWidth: true
                text: "Find regions"
                enabled: !!ana.clip.loaded
                onClicked: ana.detect("blobs", replaceBox.checked)
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: "Connected active regions of the map window: each becomes a mask ROI."
            }
            Button { text: "Clear"; enabled: ana.rois.length > 0; onClicked: ana.clearRois() }
        }
        CheckBox { id: replaceBox; text: "Replace the current ROIs"; checked: true }
        RowLayout {
            width: parent.width
            spacing: 6
            NumberField { Layout.fillWidth: true; label: "Line strength"; labelWidth: 0.55; value: page.c.lineRel; decimals: 2; step: 0.01; from: 0.01; to: 0.9; onEdited: (v) => page.num("lineRel", v); onAccepted: win.releaseFocus(); help: "Keep Hough peaks above this fraction of the strongest line. Lower = more (weaker) lines." }
            NumberField { Layout.fillWidth: true; label: "Min. length"; labelWidth: 0.5; value: page.c.lineMinLen; unit: "px"; decimals: 0; step: 10; from: 10; to: 2000; onEdited: (v) => page.num("lineMinLen", v); onAccepted: win.releaseFocus() }
        }
    }

    // ---- analysis settings -----------------------------------------------------------------------------------------------------------
    Card {
        title: "Spectrum"
        glyph: Theme.gWave
        subtitle: "Units are relative (event counts): frequencies are exact, amplitudes compare ROIs of one setup only."
        FormRow {
            label: "Signal"
            help: "Velocity: the ON−OFF rate per bin. Displacement: integrated, detrended and band-passed (smoother spectrum for long windows)."
            Segmented { fill: true; model: ["Velocity", "Displacement"]; currentIndex: page.c.signal === "displacement" ? 1 : 0; onActivated: (i) => page.num("signal", i === 1 ? "displacement" : "velocity") }
        }
        FormRow {
            label: "Window"
            help: "Hann: low leakage, for long windows. Rectangular: narrowest peak, for short pluck windows."
            Segmented { fill: true; model: ["Hann", "Rectangular"]; currentIndex: page.c.window === "rect" ? 1 : 0; onActivated: (i) => page.num("window", i === 1 ? "rect" : "hann") }
        }
        RowLayout {
            width: parent.width
            spacing: 6
            NumberField { Layout.fillWidth: true; label: "Tone"; labelWidth: 0.4; value: page.c.toneLo; unit: "Hz"; decimals: 0; step: 10; from: 1; to: 9000; onEdited: (v) => page.num("toneLo", v); onAccepted: win.releaseFocus(); help: "The tone is the strongest own peak in this band." }
            NumberField { Layout.fillWidth: true; label: "to"; labelWidth: 0.25; value: page.c.toneHi; unit: "Hz"; decimals: 0; step: 10; from: 2; to: 9900; onEdited: (v) => page.num("toneHi", v); onAccepted: win.releaseFocus() }
        }
        RowLayout {
            width: parent.width
            spacing: 6
            NumberField { Layout.fillWidth: true; label: "Band"; labelWidth: 0.4; value: page.c.bandLo; unit: "Hz"; decimals: 0; step: 10; from: 1; to: 9000; onEdited: (v) => page.num("bandLo", v); onAccepted: win.releaseFocus(); help: "Band-pass of the displacement signal and the noise reference of the SNR (peak / median of this band)." }
            NumberField { Layout.fillWidth: true; label: "to"; labelWidth: 0.25; value: page.c.bandHi; unit: "Hz"; decimals: 0; step: 10; from: 2; to: 9900; onEdited: (v) => page.num("bandHi", v); onAccepted: win.releaseFocus() }
        }
        RowLayout {
            width: parent.width
            spacing: 6
            NumberField { Layout.fillWidth: true; label: "SNR limit"; labelWidth: 0.5; value: page.c.snrMin; unit: "×"; decimals: 0; step: 1; from: 1; to: 1000; onEdited: (v) => page.num("snrMin", v); onAccepted: win.releaseFocus() }
            NumberField { Layout.fillWidth: true; label: "Bins"; labelWidth: 0.35; value: page.c.df; unit: "Hz"; decimals: 2; step: 0.05; from: 0.02; to: 10; onEdited: (v) => page.num("df", v); onAccepted: win.releaseFocus(); help: "Zero padding: the spectrum is evaluated every this many Hz (the peak is interpolated as well). It does not add resolution." }
        }
        RowLayout {
            width: parent.width
            spacing: 6
            NumberField { Layout.fillWidth: true; label: "Sampling"; labelWidth: 0.5; value: page.c.fs; unit: "Hz"; decimals: 0; step: 500; from: 500; to: 20000; onEdited: (v) => page.num("fs", v); onAccepted: win.releaseFocus(); help: "Bin rate of the ROI signals: keep it above 2.5 × the highest tone." }
            NumberField { Layout.fillWidth: true; label: "Plot to"; labelWidth: 0.4; value: page.c.fmax; unit: "Hz"; decimals: 0; step: 100; from: 50; to: 10000; onEdited: (v) => page.num("fmax", v); onAccepted: win.releaseFocus() }
        }
        FormRow {
            label: "Common mode"
            labelWidth: 0.6
            help: "With 3 or more ROIs: a tone that shows up on most ROIs but is the main peak of only a few (a mode of the whole object or the machine) is not taken as a ROI's own tone; also a tone leaking from a much stronger neighbour ROI is skipped."
            Item {
                implicitHeight: 32
                Switch { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; checked: page.c.commonMode === true; onToggled: page.num("commonMode", checked) }
            }
        }
        RowLayout {
            width: parent.width
            spacing: 6
            NumberField { Layout.fillWidth: true; label: "Spectrogram"; labelWidth: 0.55; value: page.c.sgWinMs; unit: "ms"; decimals: 0; step: 5; from: 5; to: 2000; onEdited: (v) => page.num("sgWinMs", v); onAccepted: win.releaseFocus(); help: "Frame length of the spectrogram of the selected ROI (whole clip)." }
            NumberField { Layout.fillWidth: true; label: "step"; labelWidth: 0.35; value: page.c.sgHopMs; unit: "ms"; decimals: 1; step: 1; from: 0.5; to: 500; onEdited: (v) => page.num("sgHopMs", v); onAccepted: win.releaseFocus() }
        }
    }

    // ---- rotation speed ----------------------------------------------------------------------------------------------------------------
    Card {
        id: rpmCard
        title: "Rotation speed"
        glyph: Theme.gLoop
        readonly property var sel: ana.results.find(r => r.id === ana.selRoi) || ana.results[0] || null
        info: page.c.rpmOn ? (rpmCard.sel && rpmCard.sel.rpmOk ? rpmCard.sel.name + "  " + rpmCard.sel.rpm.toFixed(1) + " rpm" : "no speed") : "off"
        subtitle: "RPM per ROI over the analysis window, from what passes it (blades, teeth, a tape mark). Use a small ROI on the path of the blade tips, not over the hub. The RPM tab below shows the speed over the whole clip."
        FormRow {
            label: "Measure RPM"
            labelWidth: 0.7
            Item {
                implicitHeight: 32
                Switch { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; checked: page.c.rpmOn === true; onToggled: page.num("rpmOn", checked) }
            }
        }
        RpmSettingsForm { cfg: page.c; set: (k, v) => page.num(k, v) }
        Repeater {
            model: page.c.rpmOn ? ana.results : []
            delegate: RowLayout {
                required property var modelData
                width: parent.width
                spacing: 8
                Rectangle { width: 10; height: 10; radius: 2; color: modelData.color }
                Label { text: modelData.name; color: Theme.text2; Layout.preferredWidth: 40 }
                Label {
                    Layout.fillWidth: true
                    text: modelData.rpmOk ? modelData.rpm.toFixed(2) + " rpm" : "–"
                    color: modelData.rpmOk ? Theme.text : Theme.text3
                    font.family: Theme.mono
                    font.weight: modelData.id === ana.selRoi ? Font.Bold : Font.Normal
                }
                Label { visible: modelData.rpmOk; text: "±" + (modelData.rpm * Math.max(modelData.rpmSpread, 1e-5)).toFixed(2); color: Theme.text3; font.pixelSize: Theme.fsSmall; font.family: Theme.mono }
            }
        }
        Hint { visible: page.c.rpmOn === true && !!rpmCard.sel && rpmCard.sel.rpmNote !== ""; text: rpmCard.sel ? rpmCard.sel.rpmNote : ""; color: rpmCard.sel && rpmCard.sel.rpmOk ? Theme.text3 : Theme.warn }
        Button {
            visible: page.c.rpmOn === true
            width: parent.width
            enabled: !!rpmCard.sel && rpmCard.sel.rpmOk === true
            text: "Strobe at this speed"
            onClicked: { player.setv("strobeHz", rpmCard.sel.rpmPass); player.setv("strobeDrift", 0.5); player.setv("strobe", true); player.setv("active", true) }
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: "Playback with one picture per pass (" + (rpmCard.sel && rpmCard.sel.rpmOk ? rpmCard.sel.rpmPass.toFixed(2) + " Hz" : "–") + "): the rotating part stands still and turns slowly with the small drift. A check by eye that the speed is right."
        }
    }

    // ---- measurements -------------------------------------------------------------------------------------------------------------------
    Card {
        title: "Measurements"
        glyph: Theme.gSave
        subtitle: "Save = ROIs + window rule + settings + results in " + (ana.clip.name || "<clip>") + ".measure/<name>/: measurement.json, results.csv, spectra.csv, signals.csv, map.png, spectra.png."
        RowLayout {
            width: parent.width
            spacing: 6
            TextField { id: mname; Layout.fillWidth: true; placeholderText: "name"; text: ana.state.measurement || ""; onAccepted: { ana.saveMeasurement(text); win.releaseFocus() } }
            Button { text: "Save"; highlighted: !!ana.state.dirty; enabled: !!ana.clip.loaded && ana.rois.length > 0; onClicked: mname.accepted() }
        }
        Label { visible: ana.measurements.length === 0; text: "No saved measurements for this clip."; color: Theme.text3; font.pixelSize: Theme.fsSmall }
        Repeater {
            model: ana.measurements
            delegate: Rectangle {
                required property var modelData
                width: parent.width
                height: mc.implicitHeight + 12
                radius: Theme.radius
                color: modelData.current ? Theme.accentSoft : Theme.field
                Column {
                    id: mc
                    x: 8; y: 6
                    width: parent.width - 16
                    RowLayout {
                        width: parent.width
                        Label { Layout.fillWidth: true; text: modelData.name; color: Theme.text; font.weight: Font.DemiBold; elide: Text.ElideRight }
                        Button { text: "Load"; flat: true; onClicked: ana.loadMeasurement(modelData.dir) }
                        IconButton { glyph: Theme.gDelete; size: 28; tip: "Delete this measurement"; onClicked: { delPop.dir = modelData.dir; delPop.name = modelData.name; delPop.open() } }
                    }
                    Label { width: parent.width; text: modelData.created + "  ·  " + modelData.rois + " ROIs  ·  " + modelData.tones + " tones" + (modelData.summary ? ":  " + modelData.summary : ""); color: Theme.text3; font.pixelSize: Theme.fsSmall; wrapMode: Text.WordWrap; maximumLineCount: 3; elide: Text.ElideRight }
                }
            }
        }
        Label { text: "Use a recipe from the project"; color: Theme.text2; font.pixelSize: Theme.fsSmall }
        ComboBox {
            id: recipeBox
            width: parent.width
            property var items: []
            function reload() { items = ana.recipes(); model = items.map(r => r.name + "  —  " + r.clip + " (" + r.rois + " ROIs, " + r.window + ")") }
            Connections { target: ana; function onClipChanged() { recipeBox.reload() } function onMeasurementsChanged() { recipeBox.reload() } }
            displayText: items.length ? "Apply…" : "no saved measurements in this project"
            enabled: items.length > 0
            onActivated: (i) => ana.applyRecipe(items[i].dir)
        }
        Hint { help: true; text: "Applying copies the ROIs and settings; save to keep the result for this clip. Run a recipe on many clips at once on the Library page (Batch)." }
    }

    FileDialog {
        id: openDlg
        title: "Open an EVS recording"
        currentFolder: lib.pathToUrl(lib.root)
        nameFilters: ["Metavision RAW (*.raw)"]
        onAccepted: ana.open(lib.urlToPath(selectedFile))
    }
    Popup {
        id: delPop
        property string dir
        property string name
        anchors.centerIn: Overlay.overlay
        modal: true
        padding: 16
        background: Rectangle { color: Theme.card; radius: Theme.radius; border.color: Theme.borderStrong }
        Column {
            spacing: 10
            width: 320
            Label { text: "Delete measurement \"" + delPop.name + "\"?"; color: Theme.text; font.weight: Font.DemiBold; width: parent.width; wrapMode: Text.WordWrap }
            Hint { text: "Its folder (JSON, CSV, PNG) is removed. The recording itself stays." }
            RowLayout {
                width: parent.width
                Button { Layout.fillWidth: true; text: "Delete"; highlighted: true; palette.accent: Theme.err; onClicked: { ana.deleteMeasurement(delPop.dir); delPop.close() } }
                Button { Layout.fillWidth: true; text: "Cancel"; onClicked: delPop.close() }
            }
        }
    }
}
