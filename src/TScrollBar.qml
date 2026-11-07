import QtQuick
import QtQuick.Controls.Basic
import Tether

// An overlay scroll bar that only shows while scrolling or hovered.
ScrollBar {
    id: control
    padding: 2
    minimumSize: 0.08
    hoverEnabled: true
    contentItem: Rectangle {
        implicitWidth: control.hovered || control.pressed ? 8 : 6
        implicitHeight: implicitWidth
        radius: width / 2
        color: Qt.rgba(1, 1, 1, control.pressed ? 0.5 : 0.35)
        opacity: control.policy === ScrollBar.AlwaysOn || (control.active && control.size < 1.0) ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 300 } }
        Behavior on implicitWidth { NumberAnimation { duration: Style.fast } }
    }
    background: null
}
