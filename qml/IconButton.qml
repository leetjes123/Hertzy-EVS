import QtQuick
import QtQuick.Controls

// square glyph button with tooltip (EVS override of qt/qml/IconButton.qml: flat, small radius)
AbstractButton {
    id: root
    property string glyph
    property string tip
    property color glyphColor: Theme.text
    property bool active: false
    property int size: 26
    implicitWidth: size
    implicitHeight: size
    hoverEnabled: true
    background: Rectangle {
        radius: Theme.radius
        color: root.active ? Theme.accent : root.down ? "#3a3d44" : root.hovered ? Theme.cardHover : "transparent"
    }
    contentItem: Label {
        text: root.glyph
        font.family: Theme.icons
        font.pixelSize: Math.max(12, Math.round(Math.min(root.size, 30) * 0.5))
        color: root.enabled ? (root.active ? "white" : root.glyphColor) : Theme.text3
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    ToolTip.visible: hovered && tip !== ""
    ToolTip.delay: 350
    ToolTip.text: tip
}
