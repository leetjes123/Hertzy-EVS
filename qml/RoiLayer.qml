import QtQuick
import QtQuick.Controls

// ROI drawing and editing on top of an EventView. Tools: select (click = select, drag = move, handles = reshape),
// line, rect, polygon (click the corners, double-click or right-click to close), brush / erase (paint a mask),
// pan, crop (live: sensor crop rectangle). Wheel = zoom, middle / right drag = pan, double-click = fit.
Item {
    id: root
    property var view                      // EventView
    property var ctl                       // app (live) or ana (analysis): rois, selRoi, addRoi, moveRoi, ...
    property string tool: "select"
    property real brushR: 6
    property var labels: ({})              // roi id -> extra text (tone)
    property bool showCrop: false
    property rect crop: Qt.rect(0, 0, 0, 0)
    property point hoverImg: Qt.point(-1, -1)
    signal cropDrawn(real x0, real y0, real x1, real y1)
    signal hovered(real x, real y)

    readonly property var rois: ctl ? ctl.rois : []
    function pt(x, y) { return view.toItem(x, y) }
    function img(x, y) { return view.toImage(x, y) }
    function repaint() { cv.requestPaint() }
    onRoisChanged: repaint()
    onToolChanged: { poly = []; drag = null; repaint() }
    onCropChanged: repaint()
    onShowCropChanged: repaint()
    Connections { target: root.view; function onViewChanged() { root.repaint(); root.viewTick++ } }
    property int viewTick: 0
    // where an ROI's name sits (item pixels); the tone labels are separate Text items, so a changing tone (8x per second
    // during playback) does not repaint the whole canvas
    function labAnchor(r) {
        if (!root.view || !r) return null
        if (r.type === "line" && r.pts.length >= 2) return pt(r.pts[0].x, r.pts[0].y)
        if ((r.type === "rect" || r.type === "polygon") && r.pts.length >= 2) return pt(r.pts[0].x, r.pts[0].y)
        if (r.type === "mask" && r.bw > 0) return pt(r.bx, r.by)
        return null
    }

    property var drag: null         // {x0, y0, x1, y1} while drawing a line / rect / crop
    property var poly: []           // polygon being drawn
    property var edit: null         // {id, mode: "move" | "handle", k, last: {x, y}, pts}
    property int strokeId: -1

    // ---- hit testing (sensor pixels) ----------------------------------------------------------------------------------
    function distSeg(px, py, ax, ay, bx, by) {
        const vx = bx - ax, vy = by - ay, L2 = vx * vx + vy * vy
        let t = L2 > 0 ? ((px - ax) * vx + (py - ay) * vy) / L2 : 0
        t = Math.max(0, Math.min(1, t))
        return Math.hypot(px - ax - t * vx, py - ay - t * vy)
    }
    function inPoly(px, py, pts) {
        let c = false
        for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) {
            const a = pts[i], b = pts[j]
            if ((a.y > py) !== (b.y > py) && px < (b.x - a.x) * (py - a.y) / (b.y - a.y) + a.x) c = !c
        }
        return c
    }
    function hitRoi(p) {
        const tol = 4 / Math.max(view.scale, 1e-6)
        for (let i = rois.length - 1; i >= 0; --i) {
            const r = rois[i]
            if (r.type === "line" && r.pts.length >= 2) { if (distSeg(p.x, p.y, r.pts[0].x, r.pts[0].y, r.pts[1].x, r.pts[1].y) <= r.half + tol) return r }
            else if (r.type === "rect" && r.pts.length >= 2) { if (p.x >= r.pts[0].x - tol && p.x <= r.pts[1].x + tol && p.y >= r.pts[0].y - tol && p.y <= r.pts[1].y + tol) return r }
            else if (r.type === "polygon") { if (inPoly(p.x, p.y, r.pts)) return r }
            else if (r.type === "mask") { if (p.x >= r.bx && p.x <= r.bx + r.bw && p.y >= r.by && p.y <= r.by + r.bh) return r }
        }
        return null
    }
    function maskAt(p) {
        for (let i = rois.length - 1; i >= 0; --i) {
            const r = rois[i]
            if (r.type === "mask" && p.x >= r.bx - brushR && p.x <= r.bx + r.bw + brushR && p.y >= r.by - brushR && p.y <= r.by + r.bh + brushR) return r
        }
        return null
    }
    function handles(r) {
        if (!r || r.type === "mask") return []
        if (r.type === "rect") return [{x: r.pts[0].x, y: r.pts[0].y}, {x: r.pts[1].x, y: r.pts[0].y}, {x: r.pts[1].x, y: r.pts[1].y}, {x: r.pts[0].x, y: r.pts[1].y}]
        return r.pts
    }
    function selected() { for (const r of rois) if (r.id === ctl.selRoi) return r; return null }
    function hitHandle(r, p) {
        const hs = handles(r), tol = 7 / Math.max(view.scale, 1e-6)
        for (let k = 0; k < hs.length; ++k) if (Math.hypot(hs[k].x - p.x, hs[k].y - p.y) <= tol) return k
        return -1
    }
    function reshaped(r, k, p) {
        if (r.type === "rect") {
            let x0 = r.pts[0].x, y0 = r.pts[0].y, x1 = r.pts[1].x, y1 = r.pts[1].y
            if (k === 0) { x0 = p.x; y0 = p.y } else if (k === 1) { x1 = p.x; y0 = p.y } else if (k === 2) { x1 = p.x; y1 = p.y } else { x0 = p.x; y1 = p.y }
            return [{x: x0, y: y0}, {x: x1, y: y1}]
        }
        const pts = r.pts.map(q => ({x: q.x, y: q.y}))
        pts[k] = {x: p.x, y: p.y}
        return pts
    }

    // ---- drawing ------------------------------------------------------------------------------------------------------------
    Repeater {
        model: root.rois
        Text {
            required property var modelData
            readonly property var a: { root.viewTick; return root.labAnchor(modelData) }
            readonly property string extra: root.labels[modelData.id] || ""
            visible: a !== null && extra !== ""
            x: a ? a.x + 5 : 0
            y: a ? a.y - 40 : 0
            text: extra
            color: modelData.color
            font.pixelSize: 13
            font.weight: Font.Bold
            style: Text.Outline
            styleColor: "#c0000000"
        }
    }
    Canvas {
        id: cv
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            if (!root.view) return
            if (root.showCrop) {
                const a = pt(root.crop.x, root.crop.y), b = pt(root.crop.x + root.crop.width, root.crop.y + root.crop.height)
                ctx.fillStyle = "rgba(0,0,0,0.45)"
                ctx.fillRule = Qt.OddEvenFill
                ctx.beginPath(); ctx.rect(0, 0, width, height); ctx.rect(a.x, a.y, b.x - a.x, b.y - a.y); ctx.fill()
                ctx.strokeStyle = "#ff4fb0"; ctx.lineWidth = 1.5; ctx.setLineDash([6, 4])
                ctx.strokeRect(a.x, a.y, b.x - a.x, b.y - a.y); ctx.setLineDash([])
            }
            ctx.font = "bold 13px '" + Theme.font + "'"
            for (const r of root.rois) {
                const sel = r.id === root.ctl.selRoi
                ctx.strokeStyle = r.color
                ctx.lineWidth = sel ? 2.2 : 1.2
                ctx.globalAlpha = sel ? 1 : 0.85
                let lab = null
                if (r.type === "line" && r.pts.length >= 2) {
                    const p0 = r.pts[0], p1 = r.pts[1], L = Math.hypot(p1.x - p0.x, p1.y - p0.y)
                    if (L < 0.5) continue
                    const nx = -(p1.y - p0.y) / L * r.half, ny = (p1.x - p0.x) / L * r.half
                    const c0 = pt(p0.x + nx, p0.y + ny), c1 = pt(p1.x + nx, p1.y + ny), c2 = pt(p1.x - nx, p1.y - ny), c3 = pt(p0.x - nx, p0.y - ny)
                    ctx.beginPath(); ctx.moveTo(c0.x, c0.y); ctx.lineTo(c1.x, c1.y); ctx.lineTo(c2.x, c2.y); ctx.lineTo(c3.x, c3.y); ctx.closePath(); ctx.stroke()
                    const a = pt(p0.x, p0.y), b = pt(p1.x, p1.y)
                    ctx.setLineDash([3, 3]); ctx.beginPath(); ctx.moveTo(a.x, a.y); ctx.lineTo(b.x, b.y); ctx.stroke(); ctx.setLineDash([])
                    lab = a
                } else if (r.type === "rect" && r.pts.length >= 2) {
                    const a = pt(r.pts[0].x, r.pts[0].y), b = pt(r.pts[1].x, r.pts[1].y)
                    ctx.strokeRect(a.x, a.y, b.x - a.x, b.y - a.y)
                    lab = a
                } else if (r.type === "polygon" && r.pts.length >= 3) {
                    ctx.beginPath()
                    for (let k = 0; k < r.pts.length; ++k) { const q = pt(r.pts[k].x, r.pts[k].y); if (k === 0) ctx.moveTo(q.x, q.y); else ctx.lineTo(q.x, q.y) }
                    ctx.closePath(); ctx.stroke()
                    lab = pt(r.pts[0].x, r.pts[0].y)
                } else if (r.type === "mask" && r.bw > 0) {
                    const a = pt(r.bx, r.by), b = pt(r.bx + r.bw, r.by + r.bh)
                    if (sel) { ctx.setLineDash([4, 4]); ctx.strokeRect(a.x, a.y, b.x - a.x, b.y - a.y); ctx.setLineDash([]) }
                    lab = a
                }
                ctx.globalAlpha = 1
                if (lab) {
                    const text = r.name
                    ctx.lineWidth = 3; ctx.strokeStyle = "rgba(0,0,0,0.75)"; ctx.strokeText(text, lab.x + 5, lab.y - 6)
                    ctx.fillStyle = r.color; ctx.fillText(text, lab.x + 5, lab.y - 6)
                }
                if (sel && root.tool === "select") {
                    ctx.fillStyle = "#ffffff"
                    for (const h of root.handles(r)) { const q = pt(h.x, h.y); ctx.fillRect(q.x - 4, q.y - 4, 8, 8) }
                }
            }
            // shape being drawn
            if (root.drag) {
                const a = pt(root.drag.x0, root.drag.y0), b = pt(root.drag.x1, root.drag.y1)
                ctx.strokeStyle = root.tool === "crop" ? "#ff4fb0" : "#ffffff"
                ctx.lineWidth = 1.5
                if (root.tool === "line") { ctx.beginPath(); ctx.moveTo(a.x, a.y); ctx.lineTo(b.x, b.y); ctx.stroke() }
                else ctx.strokeRect(a.x, a.y, b.x - a.x, b.y - a.y)
                const L = root.tool === "line" ? Math.hypot(root.drag.x1 - root.drag.x0, root.drag.y1 - root.drag.y0) : 0
                ctx.fillStyle = "#ffffff"
                ctx.font = "12px '" + Theme.mono + "'"
                ctx.fillText(root.tool === "line" ? L.toFixed(0) + " px" : Math.abs(root.drag.x1 - root.drag.x0).toFixed(0) + " × " + Math.abs(root.drag.y1 - root.drag.y0).toFixed(0) + " px", b.x + 8, b.y)
            }
            if (root.poly.length > 0) {
                ctx.strokeStyle = "#ffffff"; ctx.lineWidth = 1.5
                ctx.beginPath()
                for (let k = 0; k < root.poly.length; ++k) { const q = pt(root.poly[k].x, root.poly[k].y); if (k === 0) ctx.moveTo(q.x, q.y); else ctx.lineTo(q.x, q.y) }
                const h = pt(root.hoverImg.x, root.hoverImg.y)
                ctx.lineTo(h.x, h.y)
                ctx.stroke()
                ctx.fillStyle = "#ffffff"
                for (const q0 of root.poly) { const q = pt(q0.x, q0.y); ctx.fillRect(q.x - 3, q.y - 3, 6, 6) }
            }
            if ((root.tool === "brush" || root.tool === "erase") && root.hoverImg.x >= 0) {
                const h = pt(root.hoverImg.x, root.hoverImg.y)
                ctx.strokeStyle = root.tool === "erase" ? "#ff6161" : "#ffffff"
                ctx.lineWidth = 1
                ctx.beginPath(); ctx.arc(h.x, h.y, root.brushR * root.view.scale, 0, 2 * Math.PI); ctx.stroke()
            }
        }
    }

    // ---- interaction -------------------------------------------------------------------------------------------------------
    MouseArea {
        id: ma
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        hoverEnabled: true
        cursorShape: root.tool === "pan" || panning ? Qt.OpenHandCursor : root.tool === "select" ? Qt.ArrowCursor : (root.tool === "brush" || root.tool === "erase") ? Qt.BlankCursor : Qt.CrossCursor
        property bool panning: false
        property point last
        onPressed: (m) => {
            win.releaseFocus()
            last = Qt.point(m.x, m.y)
            const p = root.img(m.x, m.y)
            if (m.button === Qt.RightButton && root.tool === "polygon" && root.poly.length >= 3) { root.finishPoly(); return }
            if (m.button !== Qt.LeftButton || root.tool === "pan") { panning = true; return }
            if (root.tool === "select") {
                const s = root.selected()
                const k = root.hitHandle(s, p)
                if (k >= 0) { root.edit = {id: s.id, mode: "handle", k: k, r: s}; return }
                const r = root.hitRoi(p)
                root.ctl.selectRoi(r ? r.id : -1)
                if (r) root.edit = {id: r.id, mode: "move", last: {x: p.x, y: p.y}, moved: 0}
                return
            }
            if (root.tool === "brush" || root.tool === "erase") {
                // brush: grow the selected mask (else a new one); erase: the mask under the cursor, else the selected one
                const s = root.selected()
                let target = s && s.type === "mask" ? s.id : -1
                if (root.tool === "erase") { const m = root.maskAt(p); if (m) target = m.id }
                root.strokeId = root.ctl.paintMask(target, p.x, p.y, root.brushR, root.tool === "erase")
                return
            }
            if (root.tool === "polygon") {
                if (root.poly.length >= 3) {
                    const f = root.pt(root.poly[0].x, root.poly[0].y)
                    if (Math.hypot(f.x - m.x, f.y - m.y) < 9) { root.finishPoly(); return }
                }
                root.poly = root.poly.concat([{x: p.x, y: p.y}])
                root.repaint()
                return
            }
            root.drag = {x0: p.x, y0: p.y, x1: p.x, y1: p.y}
        }
        onPositionChanged: (m) => {
            const p = root.img(m.x, m.y)
            root.hoverImg = p
            root.hovered(p.x, p.y)
            if (panning) { root.view.panBy(m.x - last.x, m.y - last.y); last = Qt.point(m.x, m.y); return }
            if (root.edit && (m.buttons & Qt.LeftButton)) {
                if (root.edit.mode === "handle") root.ctl.moveRoi(root.edit.id, root.reshaped(root.edit.r, root.edit.k, p))
                else {
                    const dx = p.x - root.edit.last.x, dy = p.y - root.edit.last.y
                    const r = root.selected()
                    // masks move in whole pixels
                    const sx = r && r.type === "mask" ? Math.round(dx) : dx, sy = r && r.type === "mask" ? Math.round(dy) : dy
                    if (sx !== 0 || sy !== 0) {
                        root.ctl.translateRoi(root.edit.id, sx, sy)
                        root.edit = {id: root.edit.id, mode: "move", last: {x: root.edit.last.x + sx, y: root.edit.last.y + sy}}
                    }
                }
                return
            }
            if ((root.tool === "brush" || root.tool === "erase") && (m.buttons & Qt.LeftButton)) {
                const id = root.ctl.paintMask(root.strokeId, p.x, p.y, root.brushR, root.tool === "erase")
                if (id > 0) root.strokeId = id
            }
            if (root.drag) root.drag = {x0: root.drag.x0, y0: root.drag.y0, x1: p.x, y1: p.y}
            root.repaint()
        }
        onReleased: (m) => {
            if (panning) { panning = false; return }
            if (root.edit) { root.edit = null; return }
            if (root.tool === "brush" || root.tool === "erase") { root.ctl.endStroke(); root.strokeId = -1; return }
            const d = root.drag
            root.drag = null
            if (!d) return
            if (root.tool === "line" && Math.hypot(d.x1 - d.x0, d.y1 - d.y0) >= 4) root.ctl.addRoi("line", [{x: d.x0, y: d.y0}, {x: d.x1, y: d.y1}])
            else if (root.tool === "rect" && Math.abs(d.x1 - d.x0) > 3 && Math.abs(d.y1 - d.y0) > 3) root.ctl.addRoi("rect", [{x: d.x0, y: d.y0}, {x: d.x1, y: d.y1}])
            else if (root.tool === "crop" && Math.abs(d.x1 - d.x0) > 8 && Math.abs(d.y1 - d.y0) > 8) root.cropDrawn(d.x0, d.y0, d.x1, d.y1)
            root.repaint()
        }
        onExited: { root.hoverImg = Qt.point(-1, -1); root.repaint() }
        onWheel: (w) => root.view.zoomAt(w.x, w.y, w.angleDelta.y > 0 ? 1.25 : 0.8)
        onDoubleClicked: (m) => {
            if (root.tool === "polygon") { if (root.poly.length >= 3) root.finishPoly(); return }
            if (root.tool === "select" && root.hitRoi(root.img(m.x, m.y))) return
            root.view.fit()
        }
    }
    function finishPoly() {
        if (poly.length >= 3) ctl.addRoi("polygon", poly)
        poly = []
        repaint()
    }
}
