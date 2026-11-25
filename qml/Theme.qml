pragma Singleton
import QtQuick
import QtCore

// EVS Workbench theme: a shared Qt Quick theme re-tuned for a dense, flat, docked layout in the
// spirit of After Effects / OpenShot: neutral graphite panels, hairline separators, small radii, compact type.
// Glyphs and helpers below are a copy of the shared theme; keep them in step when adding glyphs there.
QtObject {
    // surfaces
    readonly property color bg: "#0c0d0f"          // window background = the gaps between docks
    readonly property color rail: "#17181b"        // top bar
    readonly property color panel: "#1c1d20"       // dock bodies
    readonly property color card: "#212226"        // dock / section headers
    readonly property color cardHover: "#292a2f"
    readonly property color field: "#131417"       // inset fields
    readonly property color button: "#2c2e33"
    readonly property color border: "#2a2c31"
    readonly property color borderStrong: "#3a3d44"
    readonly property color glass: "#cc17181b"
    // text
    readonly property color text: "#e6e7ea"
    readonly property color text2: "#a3a7af"
    readonly property color text3: "#6c717a"
    // accents and data colours
    readonly property color accent: "#4c9bff"
    readonly property color accentSoft: "#264c9bff"
    readonly property color ip: "#4da3ff"
    readonly property color op: "#ff7a6b"
    readonly property color pair: "#ffa23d"
    readonly property color ok: "#3fd68a"
    readonly property color warn: "#ffc23d"
    readonly property color err: "#ff6161"
    readonly property color tap: "#ff4fb0"
    // type
    // Windows 11 fonts, with the Windows 10 equivalents when they are missing (shipped packages run on both)
    readonly property var families: Qt.fontFamilies()
    function pick(list) { for (const f of list) if (families.indexOf(f) >= 0) return f; return list[list.length - 1] }
    readonly property bool win: Qt.platform.os === "windows"
    readonly property string font: win ? pick(["Segoe UI Variable Text", "Segoe UI"]) : "Noto Sans"
    readonly property string display: win ? pick(["Segoe UI Variable Display", "Segoe UI"]) : "Noto Sans"
    readonly property string icons: win ? pick(["Segoe Fluent Icons", "Segoe MDL2 Assets"]) : "DejaVu Sans"
    readonly property string mono: win ? pick(["Cascadia Mono", "Consolas"]) : "DejaVu Sans Mono"
    readonly property int fs: 12
    readonly property int fsSmall: 11
    readonly property int fsTitle: 12
    readonly property int radius: 3
    readonly property int headerH: 26               // dock and section header strips
    readonly property int gap: 2                    // between docks

    // UI state shared by every page (persisted): help text inline instead of only behind (i)
    property bool showHelp: ui.showHelp
    onShowHelpChanged: ui.showHelp = showHelp
    property Settings ui: Settings { category: "ui"; property bool showHelp: false }
    // folded sections, by key
    property var folds: ({})
    property Settings foldStore: Settings { category: "folds" }
    function folded(key, def) { if (folds[key] !== undefined) return folds[key]; const v = foldStore.value(key, def ? "1" : "0"); return v === true || v === "1" || v === "true" }
    function setFolded(key, v) { const f = Object.assign({}, folds); f[key] = v; folds = f; foldStore.setValue(key, v ? "1" : "0") }

    // glyphs (Segoe Fluent Icons)
    readonly property string gPaneOpen: win ? "" : "⇥"
    readonly property string gPaneClose: win ? "" : "⇤"
    readonly property string gHelp: win ? "" : "?"
    readonly property string gPlay: win ? "\ue768" : "▶"
    readonly property string gPause: win ? "\ue769" : "⏸"
    readonly property string gRestart: win ? "\ue72c" : "↻"
    readonly property string gOpen: win ? "\ue8e5" : "⇱"
    readonly property string gSave: win ? "\ue74e" : "⤓"
    readonly property string gCamera: win ? "\ue722" : "📷"
    readonly property string gSettings: win ? "\ue713" : "⚙"
    readonly property string gInfo: win ? "\ue946" : "ⓘ"
    readonly property string gAdd: win ? "\ue710" : "+"
    readonly property string gDelete: win ? "\ue74d" : "🗑"
    readonly property string gClose: win ? "\ue711" : "✕"
    readonly property string gChevronDown: win ? "\ue70d" : "▾"
    readonly property string gChevronUp: win ? "\ue70e" : "▴"
    readonly property string gChevronLeft: win ? "\ue76b" : "◂"
    readonly property string gChevronRight: win ? "\ue76c" : "▸"
    readonly property string gCheck: win ? "\ue73e" : "✓"
    readonly property string gView: win ? "\ue890" : "👁"
    readonly property string gHide: win ? "\ued1a" : "⊘"
    readonly property string gExport: win ? "\uede1" : "⇪"
    readonly property string gCopy: win ? "\ue8c8" : "⧉"
    readonly property string gFolder: win ? "\ue838" : "▤"
    readonly property string gZoomFit: win ? "\ue9a6" : "⤢"
    readonly property string gScreenshot: win ? "\ue924" : "▣"
    readonly property string gLoop: win ? "\ue8ee" : "↺"
    readonly property string gPin: win ? "\ue718" : "⚑"
    readonly property string gChart: win ? "\ue9d2" : "📈"
    readonly property string gTarget: win ? "\uf272" : "◎"
    readonly property string gWarning: win ? "\ue7ba" : "⚠"
    readonly property string gList: win ? "\uea37" : "☰"
    readonly property string gGrid: win ? "\uf0e2" : "▦"
    readonly property string gSweep: win ? "\ue9d9" : "≋"
    readonly property string gLink: win ? "\ue71b" : "⛓"
    readonly property string gBolt: win ? "\ue945" : "⚡"
    readonly property string gBench: win ? "\ue7c1" : "◍"
    readonly property string gRuler: win ? "\uecc6" : "⊢"
    readonly property string gWave: win ? "\ue995" : "∿"
    readonly property string gEnergy: win ? "\uebb5" : "⚡"
    readonly property string gLayers: win ? "\ue81e" : "☷"
    readonly property string gPalette: win ? "\ue790" : "◐"
    readonly property string gRecord: win ? "\ue7c8" : "●"
    readonly property string gStop: win ? "\ue71a" : "■"
    readonly property string gCube: win ? "\uf158" : "◧"
    readonly property string gEdit: win ? "" : "✎"
    readonly property string gSearch: win ? "" : "⌕"

    // config access: at(app.cfg, "/rim/erd")
    function at(obj, path) {
        var o = obj
        var parts = path.split("/")
        for (var i = 0; i < parts.length; ++i) {
            if (parts[i] === "") continue
            if (o === undefined || o === null) return undefined
            o = o[parts[i]]
        }
        return o
    }
    function fmt(v, dec) {
        if (v === undefined || v === null || isNaN(v)) return "–"
        return Number(v).toFixed(dec)
    }
}
