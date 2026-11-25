import QtQuick
import QtQuick.Controls

// secondary explanatory text (EVS override of qt/qml/Hint.qml): help = true hides it unless the help texts are on (top bar ?)
Label {
    property bool help: false
    visible: !help || Theme.showHelp
    width: parent ? parent.width : 300
    wrapMode: Text.WordWrap
    color: Theme.text3
    font.pixelSize: Theme.fsSmall
    lineHeight: 1.15
}
