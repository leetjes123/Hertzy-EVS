import QtQuick
import QtQuick.Controls

FormRow {
    id: root
    property string key
    labelWidth: 0.7
    Item {
        implicitHeight: 32
        Switch { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; checked: app.st[root.key] === true; onToggled: app.setv(root.key, checked) }
    }
}
