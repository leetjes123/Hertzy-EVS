import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// RPM settings shared by the Live and the Analyse page: what passes the ROI, the expected speed range and the window.
// cfg = the page's settings map (app.st / ana.cfg), set = function (key, value).
Column {
    id: f
    property var cfg: ({})
    property var set: function (k, v) {}
    property bool live: false
    width: parent ? parent.width : 300
    spacing: 6
    readonly property int marks: Math.max(1, f.cfg.rpmMarks || 1)

    FormRow {
        label: "Passes"
        help: "What the ROI sees once per turn. Tape: one mark per turn (a strip of tape, a bolt head, a key way). Blades: N alike marks per turn (fan blades, gear teeth, pulley arms): the speed is the pass rate / N."
        Segmented {
            fill: true
            model: ["Tape / 1 mark", "Blades / teeth"]
            currentIndex: f.marks > 1 ? 1 : 0
            onActivated: (i) => f.set("rpmMarks", i === 0 ? 1 : Math.max(2, f.cfg.rpmBlades || 3))
        }
    }
    NumberField {
        visible: f.marks > 1
        width: parent.width
        label: "Count per turn"
        value: f.marks
        decimals: 0
        step: 1
        from: 2
        to: 360
        onEdited: (v) => f.set("rpmMarks", Math.max(2, Math.round(v)))
        onAccepted: win.releaseFocus()
        help: "Number of blades / teeth / marks that pass the ROI per turn."
    }
    RowLayout {
        width: parent.width
        spacing: 6
        NumberField { Layout.fillWidth: true; label: "Range"; labelWidth: 0.42; value: f.cfg.rpmLo; unit: "rpm"; decimals: 0; step: 10; from: 1; to: 200000; onEdited: (v) => f.set("rpmLo", v); onAccepted: win.releaseFocus(); help: "Expected speed range. A narrower range avoids locking onto another periodic part of the scene." }
        NumberField { Layout.fillWidth: true; label: "to"; labelWidth: 0.25; value: f.cfg.rpmHi; unit: "rpm"; decimals: 0; step: 100; from: 2; to: 300000; onEdited: (v) => f.set("rpmHi", v); onAccepted: win.releaseFocus() }
    }
    NumberField {
        width: parent.width
        label: f.live ? "Window" : "Track frame"
        value: f.cfg.rpmWin
        unit: "s"
        decimals: 2
        step: 0.1
        from: 0.1
        to: f.live ? 10 : 30
        onEdited: (v) => f.set("rpmWin", v)
        onAccepted: win.releaseFocus()
        help: f.live ? "Events per speed reading. Longer = more precise and more stable, but slower to follow a change. It needs at least 3 passes (at 60 rpm and one mark: 3 s)."
                     : "Frame length of the speed-over-time plot of the selected ROI. The number in the table uses the whole analysis window."
    }
    NumberField {
        visible: f.live
        width: parent.width
        label: "History"
        value: f.cfg.rpmHistS
        unit: "s"
        decimals: 0
        step: 10
        from: 5
        to: 600
        onEdited: (v) => f.set("rpmHistS", v)
        onAccepted: win.releaseFocus()
    }
    Hint {
        help: true
        text: "The speed comes from the repetition of the ROI's ON−OFF signal: the lowest periodic line, refined with all its harmonics (it does not lock onto 2× or 3× the speed). Precision is roughly 0.1 / (window × harmonics) rev/s."
    }
}
