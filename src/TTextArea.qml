import QtQuick
import QtQuick.Controls.Basic
import Tether

// A multi-line field to match TField.
TextArea {
    id: control
    padding: 8
    leftPadding: 10
    font.family: Style.monoFamily
    font.pixelSize: 12
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
    }
}
