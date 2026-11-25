import QtQuick
import QtQuick.Controls

// Big speed readout over a view: rpm, the ROI it comes from, pass frequency / SNR, and the estimator's note.
Rectangle {
    id: r
    property real rpm: -1
    property bool ok: false
    property string roi: ""
    property color roiColor: "#ff4fb0"
    property real passHz: 0
    property real snr: 0
    property int marks: 1
    property string note: ""
    property string waiting: "searching…"
    width: Math.max(col.implicitWidth, 180) + 28
    height: col.implicitHeight + 18
    radius: Theme.radius
    color: "#d0111317"
    border.color: r.ok ? r.roiColor : Theme.border
    border.width: r.ok ? 2 : 1
    Column {
        id: col
        x: 14; y: 9
        spacing: 1
        Label {
            text: "SPEED" + (r.roi ? "  ·  " + r.roi : "")
            color: Theme.text3
            font.pixelSize: Theme.fsSmall
            font.letterSpacing: 1
        }
        Row {
            spacing: 6
            Label {
                text: r.ok ? r.rpm.toLocaleString(Qt.locale("en_US"), "f", r.rpm >= 1000 ? 0 : 1) : "–"
                color: r.ok ? Theme.text : Theme.text3
                font.pixelSize: 40
                font.weight: Font.DemiBold
                font.family: Theme.mono
            }
            Label {
                anchors.baseline: parent.children[0].baseline
                text: "rpm"
                color: Theme.text2
                font.pixelSize: 16
            }
        }
        Label {
            text: r.ok ? (r.rpm / 60).toFixed(r.rpm / 60 >= 100 ? 1 : 3) + " rev/s" + (r.marks > 1 ? "  ·  passes " + r.passHz.toFixed(2) + " Hz" : "") + "  ·  SNR " + r.snr.toFixed(0)
                       : r.waiting
            color: Theme.text2
            font.pixelSize: Theme.fsSmall
        }
        Label {
            visible: r.note !== ""
            width: Math.min(implicitWidth, 340)
            wrapMode: Text.WordWrap
            text: r.note
            color: r.ok ? Theme.text3 : Theme.warn
            font.pixelSize: Theme.fsSmall
        }
    }
}
