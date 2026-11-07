import QtQuick
import QtQuick.Controls.Basic
import Tether

// A macOS switch: a pill that slides its knob right and fills with the accent.
Switch {
    id: control
    padding: 0
    spacing: 8
    hoverEnabled: true
    font.family: Style.fontFamily
    font.pixelSize: Style.fontSize

    indicator: Rectangle {
        implicitWidth: 34
        implicitHeight: 20
        x: control.text ? control.width - width : 0
        anchors.verticalCenter: parent.verticalCenter
        radius: height / 2
        color: control.checked ? Style.accent : Qt.rgba(1, 1, 1, 0.16)
        Behavior on color { ColorAnimation { duration: Style.normal } }

        Rectangle {
            width: 16
            height: 16
            radius: 8
            y: 2
            x: control.checked ? parent.width - width - 2 : 2
            color: control.down ? "#E6E6E6" : "white"
            Behavior on x { NumberAnimation { duration: Style.normal; easing.type: Easing.OutCubic } }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: height / 2
            color: "transparent"
            border.width: 3
            border.color: Style.focusRing
            visible: control.visualFocus
        }
    }

    contentItem: Text {
        rightPadding: control.indicator.width + control.spacing
        text: control.text
        font: control.font
        color: Style.label
        verticalAlignment: Text.AlignVCenter
        visible: control.text !== ""
    }
}
