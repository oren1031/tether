import QtQuick
import QtQuick.Controls.Basic
import Tether

// A row in a TMenu that lights up blue under the pointer.
MenuItem {
    id: item
    implicitHeight: 26
    implicitWidth: 200
    leftPadding: 12
    rightPadding: 12
    hoverEnabled: true
    font.family: Style.fontFamily
    font.pixelSize: Style.fontSize
    contentItem: Text {
        text: item.text
        font: item.font
        color: !item.enabled ? Style.tertiaryLabel : item.highlighted ? Style.onAccent : Style.label
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: 5
        color: item.highlighted && item.enabled ? Style.accent : "transparent"
    }
    arrow: null
    indicator: null
}
