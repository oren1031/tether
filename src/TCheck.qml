import QtQuick
import QtQuick.Controls.Basic
import Tether

// A macOS checkbox: a small rounded square that fills with the accent when
// checked. On a touch screen the whole row, 44 high, takes the tap.
CheckBox {
    id: control
    property string label: ""
    font.family: Style.fontFamily
    font.pixelSize: Style.fontSize
    spacing: 8
    padding: 2
    topPadding: Style.touch ? 12 : 2
    bottomPadding: Style.touch ? 12 : 2
    implicitHeight: Math.max(Style.touch ? 44 : 0, implicitContentHeight + topPadding + bottomPadding)
    hoverEnabled: true
    opacity: enabled ? 1 : 0.4

    Accessible.name: text || label

    indicator: Rectangle {
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        anchors.verticalCenter: parent.verticalCenter
        width: Style.touch ? 20 : 16
        height: width
        radius: 4
        color: control.checked ? (control.down ? Style.accentPressed : Style.accent)
                               : control.down ? Style.fillPressed : Style.fill
        border.width: control.checked ? 0 : 1
        border.color: Style.fieldBorder
        Behavior on color { ColorAnimation { duration: Style.fast } }
        Icon {
            anchors.centerIn: parent
            name: "check"
            size: parent.width - 4
            color: Style.accentText
            visible: control.checked
        }
        TFocusRing { visible: control.visualFocus }
    }

    contentItem: Text {
        leftPadding: control.mirrored ? 0 : control.indicator.width + control.spacing
        rightPadding: control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        font: control.font
        color: Style.label
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignLeft
        verticalAlignment: Text.AlignVCenter
    }
}
