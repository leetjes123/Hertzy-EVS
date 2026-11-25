import QtQuick
import QtQuick.Controls

// small (i) glyph with a tooltip
Label {
    id: root
    property string help
    text: Theme.gInfo
    font.family: Theme.icons
    font.pixelSize: 12
    color: ma.containsMouse ? Theme.accent : Theme.text3
    MouseArea { id: ma; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true }
    ToolTip {
        visible: ma.containsMouse
        delay: 200
        text: root.help
        width: Math.min(implicitWidth, 380)
    }
}
