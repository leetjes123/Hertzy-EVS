import QtQuick
import QtQuick.Controls

// ROI / view tools as a compact strip of drawn icons (After Effects tool bar style); key in the tooltip.
// Icons are drawn here so they look the same on Windows and the Jetson (no icon font needed).
Rectangle {
    id: root
    property var tools: ["select", "line", "rect", "polygon", "brush", "erase", "pan"]
    property string current: "select"
    signal picked(string tool)
    readonly property var names: ({select: "Select / move / reshape  [V]", line: "Line ROI  [L]", rect: "Rectangle ROI  [R]",
                                   polygon: "Polygon ROI  [G]", brush: "Brush ROI  [B]", erase: "Erase brush  [E]",
                                   pan: "Pan  [H]  (wheel = zoom, double-click = fit)", crop: "Crop: drag the sensor region to keep"})
    implicitWidth: row.implicitWidth + 4
    implicitHeight: 28
    radius: Theme.radius
    color: Theme.field
    border.color: Theme.border
    Row {
        id: row
        anchors.centerIn: parent
        spacing: 1
        Repeater {
            model: root.tools
            delegate: AbstractButton {
                id: b
                required property string modelData
                readonly property bool sel: root.current === modelData
                width: 28; height: 24
                hoverEnabled: true
                onClicked: root.picked(modelData)
                background: Rectangle {
                    radius: Theme.radius - 1
                    color: b.sel ? Theme.accent : b.hovered ? Theme.cardHover : "transparent"
                }
                contentItem: Canvas {
                    id: cv
                    readonly property color ink: b.sel ? "white" : Theme.text2
                    onInkChanged: requestPaint()
                    onPaint: {
                        const g = getContext("2d")
                        g.reset()
                        g.strokeStyle = ink; g.fillStyle = ink; g.lineWidth = 1.5; g.lineJoin = "round"; g.lineCap = "round"
                        const w = width, h = height, cx = w / 2, cy = h / 2
                        g.translate(cx - 8, cy - 8)   // 16 x 16 icon box
                        switch (b.modelData) {
                        case "select":
                            g.beginPath(); g.moveTo(4, 2); g.lineTo(4, 14); g.lineTo(7, 11); g.lineTo(9.5, 15); g.lineTo(11, 14.2)
                            g.lineTo(8.6, 10.3); g.lineTo(12.5, 10.3); g.closePath(); g.fill(); break
                        case "line":
                            g.beginPath(); g.moveTo(3, 13); g.lineTo(13, 3); g.stroke()
                            g.beginPath(); g.arc(3, 13, 1.8, 0, 7); g.fill(); g.beginPath(); g.arc(13, 3, 1.8, 0, 7); g.fill(); break
                        case "rect":
                            g.strokeRect(2.5, 4, 11, 8); break
                        case "polygon":
                            g.beginPath(); g.moveTo(3, 6); g.lineTo(9, 2.5); g.lineTo(14, 7); g.lineTo(11, 13.5); g.lineTo(4, 12); g.closePath(); g.stroke(); break
                        case "brush":
                            g.beginPath(); g.moveTo(13.5, 2.5); g.lineTo(7, 9); g.stroke()
                            g.beginPath(); g.moveTo(6.5, 8.5); g.quadraticCurveTo(2, 9, 2.5, 14); g.quadraticCurveTo(7.5, 14, 7.5, 9.5); g.closePath(); g.fill(); break
                        case "erase":
                            g.beginPath(); g.moveTo(2, 10); g.lineTo(8, 4); g.lineTo(13, 9); g.lineTo(9, 13); g.lineTo(5, 13); g.closePath(); g.stroke()
                            g.beginPath(); g.moveTo(5, 7); g.lineTo(10, 12); g.stroke()
                            g.beginPath(); g.moveTo(9, 13.5); g.lineTo(15, 13.5); g.stroke(); break
                        case "pan":
                            g.beginPath(); g.moveTo(8, 1.5); g.lineTo(8, 14.5); g.moveTo(1.5, 8); g.lineTo(14.5, 8); g.stroke()
                            g.beginPath(); g.moveTo(8, 1); g.lineTo(6, 3.5); g.lineTo(10, 3.5); g.closePath(); g.fill()
                            g.beginPath(); g.moveTo(8, 15); g.lineTo(6, 12.5); g.lineTo(10, 12.5); g.closePath(); g.fill()
                            g.beginPath(); g.moveTo(1, 8); g.lineTo(3.5, 6); g.lineTo(3.5, 10); g.closePath(); g.fill()
                            g.beginPath(); g.moveTo(15, 8); g.lineTo(12.5, 6); g.lineTo(12.5, 10); g.closePath(); g.fill(); break
                        case "crop":
                            g.beginPath(); g.moveTo(4, 1); g.lineTo(4, 12); g.lineTo(15, 12); g.moveTo(1, 4); g.lineTo(12, 4); g.lineTo(12, 15); g.stroke(); break
                        }
                    }
                }
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: root.names[modelData] || modelData
            }
        }
    }
}
