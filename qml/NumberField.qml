import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Numeric input with unit suffix. Typing commits on Enter / focus loss; arrow keys and the mouse wheel (when focused)
// step the value. Optional "auto" state (value derived by the model) with a toggle to override / go back to auto.
FormRow {
    id: root
    property real value: 0
    property string unit
    property int decimals: 1
    property real step: 1
    property real from: -1e12
    property real to: 1e12
    property bool allowAuto: false
    property bool isAuto: false
    property real autoValue: NaN
    property bool sci: false
    signal edited(real v)
    signal autoRequested()
    signal accepted()      // Enter was pressed in the field

    function show(v) {
        if (v === undefined || v === null || isNaN(v)) return ""
        return root.sci ? Number(v).toExponential(2) : Number(v).toFixed(root.decimals)
    }
    function commit(v) {
        if (isNaN(v)) { tf.text = show(root.value); return }
        v = Math.max(root.from, Math.min(root.to, v))
        root.edited(v)
    }

    RowLayout {
        spacing: 6
        TextField {
            id: tf
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            horizontalAlignment: Text.AlignRight
            rightPadding: unitLbl.visible ? unitLbl.implicitWidth + 16 : 10
            font.pixelSize: Theme.fs
            color: root.isAuto ? Theme.text3 : Theme.text
            placeholderText: root.isAuto ? "auto  " + root.show(root.autoValue) : ""
            selectByMouse: true
            text: root.isAuto ? "" : root.show(root.value)
            onAccepted: root.accepted()
            onEditingFinished: {
                if (text.trim() === "" && root.allowAuto) { root.autoRequested(); return }
                root.commit(parseFloat(text.replace(",", ".")))
                text = Qt.binding(() => root.isAuto ? "" : root.show(root.value))
            }
            onActiveFocusChanged: if (activeFocus) {
                text = root.isAuto ? root.show(root.autoValue) : root.show(root.value)
                selectAll()
            }
            Keys.onUpPressed: root.commit((root.isAuto ? root.autoValue : root.value) + root.step)
            Keys.onDownPressed: root.commit((root.isAuto ? root.autoValue : root.value) - root.step)
            Label {
                id: unitLbl
                visible: root.unit !== ""
                text: root.unit
                color: Theme.text3
                font.pixelSize: Theme.fsSmall
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        Button {
            visible: root.allowAuto
            Layout.preferredHeight: 32
            Layout.preferredWidth: 60
            text: root.isAuto ? "Auto" : "Set"
            font.pixelSize: Theme.fsSmall
            highlighted: root.isAuto
            ToolTip.visible: hovered
            ToolTip.delay: 300
            ToolTip.text: root.isAuto ? "Derived by the model — click to enter your own value." : "Click to let the model derive this value again."
            onClicked: root.isAuto ? root.edited(isNaN(root.autoValue) ? root.value : root.autoValue) : root.autoRequested()
        }
    }
}
