import QtQuick
import QtQuick.Controls.Basic
import Tether

// A macOS checkbox: a small rounded square that fills blue when checked.
CheckBox {
    id: control
    font.family: Style.fontFamily
    font.pixelSize: Style.fontSize
    spacing: 8
    padding: 2
    hoverEnabled: true

    indicator: Rectangle {
        x: control.leftPadding
        anchors.verticalCenter: parent.verticalCenter
        width: 16
        height: 16
        radius: 4
        color: control.checked ? (control.down ? Style.accentPressed : Style.accent)
                               : control.down ? Style.fillPressed : Style.fill
        border.width: control.checked ? 0 : 1
        border.color: Qt.rgba(1, 1, 1, 0.18)
        Behavior on color { ColorAnimation { duration: Style.fast } }
        Icon {
            anchors.centerIn: parent
            name: "check"
            size: 12
            visible: control.checked
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: 7
            color: "transparent"
            border.width: 3
            border.color: Style.focusRing
            visible: control.visualFocus
        }
    }

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        font: control.font
        color: Style.label
        wrapMode: Text.Wrap
        verticalAlignment: Text.AlignVCenter
    }
}
