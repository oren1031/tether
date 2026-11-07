import QtQuick
import QtQuick.Controls.Basic
import Tether

// A rounded text field with a blue focus ring. iconName puts an icon inside
// on the left, like a search field.
TextField {
    id: control
    property string iconName: ""

    implicitHeight: Style.controlHeight + 2
    leftPadding: iconName ? 30 : 10
    rightPadding: 10
    topPadding: 0
    bottomPadding: 0
    verticalAlignment: TextInput.AlignVCenter
    font.family: Style.fontFamily
    font.pixelSize: Style.fontSize
    color: Style.label
    placeholderTextColor: Style.tertiaryLabel
    selectionColor: Style.tint(0.45)
    selectedTextColor: "white"
    selectByMouse: true

    background: Rectangle {
        radius: 7
        color: Style.fieldFill
        border.width: 1
        border.color: control.activeFocus ? Style.tint(0.9) : Style.fieldBorder
        Behavior on border.color { ColorAnimation { duration: Style.fast } }

        Icon {
            visible: control.iconName !== ""
            name: control.iconName
            size: 14
            opacity: 0.45
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
        }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: parent.radius + 3
            color: "transparent"
            border.width: 3
            border.color: Style.focusRing
            opacity: control.activeFocus ? 0.6 : 0
            Behavior on opacity { NumberAnimation { duration: Style.fast } }
        }
    }
}
