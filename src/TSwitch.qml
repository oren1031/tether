import QtQuick
import QtQuick.Controls.Basic
import Tether

// A macOS switch: a pill that slides its knob across and fills with the
// accent. On a touch screen it's bigger, with room around it to tap.
Switch {
    id: control
    property string label: ""
    padding: 0
    topPadding: Style.touch ? (44 - indicator.height) / 2 : 0
    bottomPadding: topPadding
    leftPadding: Style.touch && !text ? 2 : 0
    rightPadding: leftPadding
    spacing: 8
    hoverEnabled: true
    font.family: Style.fontFamily
    font.pixelSize: Style.fontSize
    opacity: enabled ? 1 : 0.4

    Accessible.name: text || label

    indicator: Rectangle {
        implicitWidth: Style.touch ? 44 : 34
        implicitHeight: Style.touch ? 26 : 20
        x: control.text ? (control.mirrored ? control.leftPadding : control.width - width - control.rightPadding)
                        : control.leftPadding
        anchors.verticalCenter: parent.verticalCenter
        radius: height / 2
        color: control.checked ? Style.accent : Qt.rgba(1, 1, 1, 0.16)
        // Off, the fill alone is too faint to find; the edge isn't.
        border.width: control.checked ? 0 : 1
        border.color: Style.fieldBorder
        Behavior on color { ColorAnimation { duration: Style.normal } }

        Rectangle {
            readonly property bool atEnd: control.checked !== control.mirrored
            width: parent.height - 4
            height: width
            radius: width / 2
            y: 2
            x: atEnd ? parent.width - width - 2 : 2
            color: control.down ? "#E6E6E6" : "white"
            Behavior on x { NumberAnimation { duration: Style.normal; easing.type: Easing.OutCubic } }
        }
        TFocusRing { visible: control.visualFocus }
    }

    contentItem: Text {
        leftPadding: control.mirrored ? control.indicator.width + control.spacing : 0
        rightPadding: control.mirrored ? 0 : control.indicator.width + control.spacing
        text: control.text
        font: control.font
        color: Style.label
        horizontalAlignment: Text.AlignLeft
        verticalAlignment: Text.AlignVCenter
        visible: control.text !== ""
    }
}
