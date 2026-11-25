import QtQuick
import QtQuick.Controls

// TableView for the controller's TableModel: header with sorting, selectable rows, coloured / dimmed cells, checkboxes
Item {
    id: root
    property var model
    property bool sortable: false
    property int sortCol: 0
    property bool sortAsc: true
    signal rowClicked(int key)
    signal checkToggled(int key, bool on)
    signal sortRequested(int col, bool asc)
    implicitHeight: 360
    clip: true

    readonly property real naturalWidth: {
        var s = 0
        if (!model) return 1
        var dep = model.rows
        for (var c = 0; c < model.columnCount(); ++c) s += model.columnWidth(c)
        return Math.max(s, 1)
    }
    readonly property real k: Math.max(0.78, (width - 14) / naturalWidth)

    Rectangle { anchors.fill: parent; color: "#16191d"; radius: 8; border.color: Theme.border }

    HorizontalHeaderView {
        id: header
        x: 1; y: 1
        width: parent.width - 2
        syncView: tv
        clip: true
        delegate: Rectangle {
            required property string display
            required property int column
            implicitHeight: 32
            color: "#1d2127"
            Label {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 6
                verticalAlignment: Text.AlignVCenter
                text: parent.display + (root.sortable && root.sortCol === parent.column ? (root.sortAsc ? "  ▲" : "  ▼") : "")
                color: Theme.text2
                font.pixelSize: Theme.fsSmall
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
            MouseArea {
                anchors.fill: parent
                enabled: root.sortable
                cursorShape: root.sortable ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: {
                    if (root.sortCol === parent.column) root.sortAsc = !root.sortAsc
                    else { root.sortCol = parent.column; root.sortAsc = true }
                    root.sortRequested(root.sortCol, root.sortAsc)
                }
            }
        }
    }
    TableView {
        id: tv
        x: 1
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 1
        width: parent.width - 2
        clip: true
        model: root.model
        boundsBehavior: Flickable.StopAtBounds
        columnWidthProvider: (c) => root.model ? root.model.columnWidth(c) * root.k : 80
        rowHeightProvider: () => 30
        onWidthChanged: forceLayout()
        Connections { target: root.model; function onChanged() { tv.forceLayout() } }
        ScrollBar.vertical: ScrollBar {}
        delegate: Rectangle {
            required property string display
            required property var cellColor
            required property int kind
            required property bool checked
            required property bool dim
            required property int rowKey
            required property bool selected
            required property int row
            implicitHeight: 30
            color: selected ? Theme.accentSoft : (row % 2 ? "#1a1d22" : "transparent")
            Label {
                visible: parent.kind === 0
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 6
                verticalAlignment: Text.AlignVCenter
                text: parent.display
                color: parent.cellColor ? parent.cellColor : (parent.dim ? Theme.text3 : Theme.text)
                font.pixelSize: Theme.fs
                elide: Text.ElideRight
            }
            CheckBox {
                visible: parent.kind === 1
                anchors.verticalCenter: parent.verticalCenter
                x: 2
                scale: 0.8
                checked: parent.checked
                onToggled: root.checkToggled(parent.rowKey, checked)
            }
            MouseArea {
                anchors.fill: parent
                visible: parent.kind === 0
                cursorShape: Qt.PointingHandCursor
                onClicked: root.rowClicked(parent.rowKey)
            }
        }
    }
    function positionAtKey(key) {
        var r = root.model ? root.model.rowOfKey(key) : -1
        if (r >= 0) tv.positionViewAtRow(r, TableView.Contain)
    }
}
