import QtQuick
import QtQuick.Controls

// dock body: fold-out sections stacked edge to edge (EVS override of qt/qml/PageScroll.qml)
ScrollView {
    id: sv
    default property alias content: col.data
    contentWidth: availableWidth
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    // scripted screenshots: --scroll 0..1 scrolls the page before the grab
    Timer {
        running: scripted && startScroll > 0
        interval: 700
        repeat: true
        onTriggered: sv.contentItem.contentY = startScroll * Math.max(0, sv.contentHeight - sv.height)
    }
    Column {
        id: col
        width: sv.availableWidth
        spacing: 0
        bottomPadding: 16
    }
}
