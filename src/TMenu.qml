import QtQuick
import QtQuick.Controls.Basic
import Tether

// A macOS context menu: a rounded translucent panel whose rows light up blue.
Menu {
    id: menu
    padding: 5
    // Kept clear of a phone's status and navigation bars.
    topMargin: Style.safeTop
    bottomMargin: Style.safeBottom
    leftMargin: Style.safeLeft
    rightMargin: Style.safeRight
    font.family: Style.fontFamily
    font.pixelSize: Style.fontSize

    delegate: TMenuItem {}

    background: Rectangle {
        implicitWidth: 200
        radius: Style.radius + 1
        color: Style.popover
        border.color: Qt.rgba(1, 1, 1, 0.12)
    }

    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Style.fast } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 80 } }
}
