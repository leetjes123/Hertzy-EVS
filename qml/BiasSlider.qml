import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// slider for one camera setting: sends while dragging (throttled), shows the value and an optional model note
FormRow {
    id: root
    property string key
    property real from: 0
    property real to: 100
    property real step: 1
    property int decimals: 0
    property string unit
    property string note
    readonly property real value: Number(app.st[key])
    property real pending: NaN
    labelWidth: 0.36

    Timer { id: throttle; interval: 45; onTriggered: if (!isNaN(root.pending)) { app.setv(root.key, root.pending); root.pending = NaN } }

    RowLayout {
        spacing: 6
        Slider {
            id: sl
            Layout.fillWidth: true
            from: root.from
            to: root.to
            stepSize: root.step
            snapMode: Slider.SnapAlways
            value: root.value
            onMoved: { root.pending = value; if (!throttle.running) throttle.start() }
            onPressedChanged: if (!pressed) { throttle.stop(); app.setv(root.key, value); root.pending = NaN }
        }
        Label {
            Layout.preferredWidth: 74
            horizontalAlignment: Text.AlignRight
            font.family: Theme.mono
            font.pixelSize: Theme.fsSmall
            color: Theme.text
            text: (sl.value > 0 && root.from < 0 ? "+" : "") + sl.value.toFixed(root.decimals) + (root.unit ? " " + root.unit : "")
                  + (root.note ? "\n" + root.note : "")
            lineHeight: 0.95
        }
    }
}
