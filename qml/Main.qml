import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: win
    visible: true
    width: startSize.width
    height: startSize.height
    minimumWidth: 1200
    minimumHeight: 720
    readonly property bool viewer: edition === "viewer"
    readonly property string appName: viewer ? "HertzyEVS Viewer" : "HertzyEVS"
    title: viewer ? appName : appName + " — " + (app.live.source || "no camera")
    color: Theme.bg
    font.family: Theme.font
    palette.accent: Theme.accent
    palette.highlight: Theme.accent
    font.pixelSize: Theme.fs
    Component.onCompleted: {
        {
            // Fusion (every platform since the flat EVS look) draws fields and buttons from the palette, which is light without a dark desktop theme
            palette.window = Theme.panel
            palette.windowText = Theme.text
            palette.base = Theme.field
            palette.alternateBase = Theme.card
            palette.text = Theme.text
            palette.button = Theme.button
            palette.buttonText = Theme.text
            palette.brightText = "white"
            palette.placeholderText = Theme.text3
            palette.highlightedText = "white"
            palette.toolTipBase = Theme.card
            palette.toolTipText = Theme.text
            palette.light = Theme.borderStrong
            palette.midlight = "#33363c"
            palette.mid = Theme.border
            palette.dark = Theme.bg
            palette.shadow = "black"
            palette.disabled.text = Theme.text3
            palette.disabled.buttonText = Theme.text3
            palette.disabled.windowText = Theme.text3
        }
        if (!scripted) showMaximized()
    }

    // the viewer edition has no recording part: Library and Analyse only
    readonly property var pages: [
        {key: "library", title: "Library", glyph: Theme.gFolder, group: 0},
        {key: "live", title: "Live", glyph: Theme.gRecord, group: 0},
        {key: "analyse", title: "Analyse", glyph: Theme.gChart, group: 0},
        {key: "camera", title: "Camera", glyph: Theme.gCamera, group: 0}
    ].filter(p => !viewer || p.key === "library" || p.key === "analyse")
    function goPage(i) { if (i < pages.length) app.setv("page", pages[i].key) }
    readonly property var current: pages.find(p => p.key === app.st.page) || pages[0]
    readonly property string pageName: current.key.charAt(0).toUpperCase() + current.key.slice(1)
    readonly property bool onLive: app.st.page === "live"
    readonly property bool onAnalyse: app.st.page === "analyse"

    function releaseFocus() { focusSink.forceActiveFocus() }
    function openClip(path) { ana.open(path); app.setv("page", "analyse") }
    // tool switching for the page that draws ROIs
    function setTool(t) { if (onLive) app.setv("tool", t); else if (onAnalyse) ana.setCfg("tool", t) }
    function roiCtl() { return onLive ? app : ana }
    Item { id: focusSink }

    // blinking star chart for focusing (Live > Setup aids); lives here so it stays open when the page changes
    FocusChart {
        id: focusChart
        rays: app.st.chartRays || 36
        hz: app.st.chartHz || 4
        invert: app.st.chartInvert !== false
        onSettingsChanged: (s, h, inv) => { app.setv("chartRays", s); app.setv("chartHz", h); app.setv("chartInvert", inv) }
    }

    Connections { target: app; function onToast(text) { toast.show(text) } }
    Connections { target: lib; function onToast(text) { toast.show(text) } }
    Connections { target: ana; function onToast(text) { toast.show(text) } }
    Connections { target: player; function onToast(text) { toast.show(text) } }

    // keys (text fields keep their own keys)
    Shortcut { sequence: "Space"; enabled: win.onLive; onActivated: app.recordNow() }
    Shortcut { sequence: "A"; enabled: win.onLive; onActivated: app.toggleArm() }
    // playback (Analyse > Playback)
    readonly property bool playKeys: win.onAnalyse && player.cfg.active === true
    Shortcut { sequence: "Space"; enabled: win.playKeys; onActivated: player.toggle() }
    Shortcut { sequence: "Left"; enabled: win.playKeys; onActivated: player.step(-1) }
    Shortcut { sequence: "Right"; enabled: win.playKeys; onActivated: player.step(1) }
    Shortcut { sequence: "Home"; enabled: win.playKeys; onActivated: player.home() }
    Shortcut { sequence: "Esc"; onActivated: { if (win.onLive) app.cancelRecording() } }
    Shortcut { sequence: "T"; enabled: win.onLive && app.st.source === "sim"; onActivated: app.simTap() }
    Shortcut { sequence: "V"; enabled: win.onLive || win.onAnalyse; onActivated: win.setTool("select") }
    Shortcut { sequence: "L"; enabled: win.onLive || win.onAnalyse; onActivated: win.setTool("line") }
    Shortcut { sequence: "R"; enabled: win.onLive || win.onAnalyse; onActivated: win.setTool("rect") }
    Shortcut { sequence: "G"; enabled: win.onLive || win.onAnalyse; onActivated: win.setTool("polygon") }
    Shortcut { sequence: "B"; enabled: win.onLive || win.onAnalyse; onActivated: win.setTool("brush") }
    Shortcut { sequence: "E"; enabled: win.onLive || win.onAnalyse; onActivated: win.setTool("erase") }
    Shortcut { sequence: "H"; enabled: win.onLive || win.onAnalyse; onActivated: win.setTool("pan") }
    Shortcut { sequence: "Delete"; enabled: win.onLive || win.onAnalyse; onActivated: { const c = win.roiCtl(); if (c.selRoi > 0) c.removeRoi(c.selRoi) } }
    Shortcut { sequence: "Ctrl+D"; enabled: win.onLive || win.onAnalyse; onActivated: { const c = win.roiCtl(); if (c.selRoi > 0) c.duplicateRoi(c.selRoi) } }
    Shortcut { sequence: "Ctrl+S"; enabled: win.onAnalyse; onActivated: saveDlg.open() }
    Shortcut { sequence: "1"; onActivated: win.goPage(0) }
    Shortcut { sequence: "2"; onActivated: win.goPage(1) }
    Shortcut { sequence: "3"; onActivated: win.goPage(2) }
    Shortcut { sequence: "4"; onActivated: win.goPage(3) }

    // side panel per page: shown or hidden (\ or the top-bar button), remembered
    readonly property bool sideOpen: !Theme.folded("side/" + current.key, false)
    function toggleSide() { Theme.setFolded("side/" + current.key, sideOpen) }
    Shortcut { sequence: "\\"; onActivated: win.toggleSide() }

    header: EvsTopBar {
        pages: win.pages
        sideOpen: win.sideOpen
        onToggleSide: win.toggleSide()
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal
        handle: Rectangle {
            implicitWidth: Theme.gap + 2
            color: SplitHandle.pressed ? Theme.accent : (SplitHandle.hovered ? Theme.borderStrong : Theme.bg)
        }
        Rectangle {
            visible: win.sideOpen
            SplitView.preferredWidth: 320
            SplitView.minimumWidth: 260
            SplitView.maximumWidth: 640
            color: Theme.panel
            Loader {
                anchors.fill: parent
                source: win.pageName + "Side.qml"
            }
        }
        Rectangle {
            SplitView.fillWidth: true
            color: Theme.bg
            Loader {
                anchors.fill: parent
                anchors.margins: Theme.gap
                source: win.pageName + "View.qml"
            }
        }
    }

    // save a measurement (Ctrl+S on the analysis page)
    Popup {
        id: saveDlg
        anchors.centerIn: parent
        modal: true
        padding: 16
        background: Rectangle { color: Theme.card; radius: Theme.radius; border.color: Theme.borderStrong }
        onOpened: { nameField.text = ana.state.measurement || "measurement"; nameField.selectAll(); nameField.forceActiveFocus() }
        Column {
            spacing: 10
            width: 340
            Label { text: "Save measurement"; color: Theme.text; font.weight: Font.DemiBold }
            Hint { text: "ROIs, window, settings and results go to " + (ana.clip.name || "") + ".measure/<name>/ (JSON, CSV, PNG). The same name overwrites." }
            TextField { id: nameField; width: parent.width; onAccepted: { ana.saveMeasurement(text); saveDlg.close() } }
            RowLayout {
                width: parent.width
                Button { Layout.fillWidth: true; text: "Save"; highlighted: true; onClicked: nameField.accepted() }
                Button { Layout.fillWidth: true; text: "Cancel"; onClicked: saveDlg.close() }
            }
        }
    }
    function saveMeasurementDialog() { saveDlg.open() }

    Rectangle {
        id: toast
        property string text
        function show(t) { text = t; opacity = 1; hideTimer.restart() }
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 48   // just below the page toolbar, over the top of the picture
        width: Math.min(tl.implicitWidth + 32, parent.width - 80)
        height: 32
        radius: Theme.radius
        z: 100
        color: "#f02a2f37"
        border.color: Theme.borderStrong
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        Label { id: tl; anchors.centerIn: parent; text: toast.text; color: Theme.text; elide: Text.ElideMiddle; width: Math.min(implicitWidth, toast.width - 32) }
        Timer { id: hideTimer; interval: 4500; onTriggered: toast.opacity = 0 }
    }
}
