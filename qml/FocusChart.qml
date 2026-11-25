import QtQuick
import QtQuick.Window

// Blinking Siemens-star focus chart for an event camera. Put this window on a screen in front of the camera (full
// screen), point the camera at it and turn the focus ring: an event camera only sees changes, so the chart blinks
// (chart <-> its negative, or chart <-> black). In focus the rays stay separate closest to the star centres: the
// grey blur disc in the middle of each star is smallest. The four corner stars show focus over the field.
// Keys in the chart window: Esc = close, F = full screen on / off, Up / Down = faster / slower, Space = pause.
Window {
    id: w
    title: "Focus chart"
    color: "black"
    width: 1280
    height: 800
    visible: false

    property int rays: 36
    property real hz: 4              // full cycles per second (two flips)
    property bool invert: true       // true: chart <-> negative (twice the events); false: chart <-> black
    property bool paused: false
    property bool phase: false
    property bool showHelp: true
    signal settingsChanged(int rays, real hz, bool invert)

    function showOn(screenIndex, full) {
        const sc = Qt.application.screens
        if (screenIndex >= 0 && screenIndex < sc.length) {
            w.screen = sc[screenIndex]
            w.x = sc[screenIndex].virtualX + 40
            w.y = sc[screenIndex].virtualY + 40
        }
        showHelp = true
        helpTimer.restart()
        if (full) w.showFullScreen(); else w.showNormal()
        w.raise()
        w.requestActivate()
    }

    onRaysChanged: { pos.requestPaint(); neg.requestPaint() }
    onWidthChanged: { pos.requestPaint(); neg.requestPaint() }
    onHeightChanged: { pos.requestPaint(); neg.requestPaint() }

    Timer {
        interval: Math.max(8, 500 / Math.max(0.1, w.hz))
        running: w.visible && !w.paused && w.hz > 0
        repeat: true
        onTriggered: w.phase = !w.phase
    }
    Timer { id: helpTimer; interval: 4000; onTriggered: w.showHelp = false }

    // one star: n black / white wedge pairs
    function star(ctx, cx, cy, r, n, fg) {
        ctx.fillStyle = fg
        const da = Math.PI / n
        for (let k = 0; k < n; ++k) {
            const a0 = 2 * Math.PI * k / n
            ctx.beginPath()
            ctx.moveTo(cx, cy)
            ctx.arc(cx, cy, r, a0, a0 + da, false)
            ctx.closePath()
            ctx.fill()
        }
    }
    function drawChart(ctx, W, H, negative) {
        const bg = negative ? "white" : "black", fg = negative ? "black" : "white"
        ctx.reset()
        ctx.fillStyle = bg
        ctx.fillRect(0, 0, W, H)
        const m = Math.min(W, H)
        // centre star + four corner stars (field focus / tilt)
        star(ctx, W / 2, H / 2, 0.40 * m, w.rays, fg)
        const rc = 0.16 * m, pad = 0.03 * m
        for (const c of [[pad + rc, pad + rc], [W - pad - rc, pad + rc], [pad + rc, H - pad - rc], [W - pad - rc, H - pad - rc]])
            star(ctx, c[0], c[1], rc, Math.max(8, Math.round(w.rays * 0.75)), fg)
        // centre marks on the edges
        ctx.fillStyle = fg
        ctx.fillRect(W / 2 - 1, 0, 2, 0.02 * m)
        ctx.fillRect(W / 2 - 1, H - 0.02 * m, 2, 0.02 * m)
        ctx.fillRect(0, H / 2 - 1, 0.02 * m, 2)
        ctx.fillRect(W - 0.02 * m, H / 2 - 1, 0.02 * m, 2)
    }

    Canvas {
        id: pos
        anchors.fill: parent
        onPaint: w.drawChart(getContext("2d"), width, height, false)
    }
    Canvas {
        id: neg
        anchors.fill: parent
        visible: w.phase && w.invert
        onPaint: w.drawChart(getContext("2d"), width, height, true)
    }
    Rectangle {
        anchors.fill: parent
        color: "black"
        visible: w.phase && !w.invert
    }

    // short help, fades out (it would make events too)
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 30
        width: helpText.implicitWidth + 30
        height: helpText.implicitHeight + 16
        radius: 8
        color: "#e0202428"
        opacity: w.showHelp ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 400 } }
        Text {
            id: helpText
            anchors.centerIn: parent
            color: "#e6e8eb"
            font.pixelSize: 16
            text: w.rays + " rays · " + w.hz.toFixed(1) + " Hz" + (w.invert ? " · chart ↔ negative" : " · chart ↔ black") + (w.paused ? " · paused" : "")
                  + "\nEsc close · F full screen · ↑ ↓ blink rate · ← → rays · I invert · Space pause"
            horizontalAlignment: Text.AlignHCenter
        }
    }

    Item {
        anchors.fill: parent
        focus: true
        function bump() { w.showHelp = true; helpTimer.restart(); w.settingsChanged(w.rays, w.hz, w.invert) }
        Keys.onPressed: (e) => {
            if (e.key === Qt.Key_Escape) w.close()
            else if (e.key === Qt.Key_F) { if (w.visibility === Window.FullScreen) w.showNormal(); else w.showFullScreen() }
            else if (e.key === Qt.Key_Up) { w.hz = Math.min(30, w.hz * 1.5); bump() }
            else if (e.key === Qt.Key_Down) { w.hz = Math.max(0.5, w.hz / 1.5); bump() }
            else if (e.key === Qt.Key_Right) { w.rays = Math.min(144, w.rays + 4); bump() }
            else if (e.key === Qt.Key_Left) { w.rays = Math.max(8, w.rays - 4); bump() }
            else if (e.key === Qt.Key_I) { w.invert = !w.invert; bump() }
            else if (e.key === Qt.Key_Space) { w.paused = !w.paused; bump() }
            else return
            e.accepted = true
        }
        MouseArea {
            anchors.fill: parent
            onClicked: { w.showHelp = true; helpTimer.restart() }
            onDoubleClicked: { if (w.visibility === Window.FullScreen) w.showNormal(); else w.showFullScreen() }
        }
    }
}
