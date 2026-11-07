import QtQuick
import QtQuick.Controls.Basic
import Tether

// A macOS push button. kind is "default" (gray), "primary" (blue), "plain"
// (text only), "danger" (red), or "toolbar" (an icon that lights up on hover).
Button {
    id: control
    property string kind: highlighted ? "primary" : flat ? "plain" : "default"
    property string iconName: ""
    property int iconSize: 15

    readonly property bool filled: kind === "primary" || kind === "danger"

    implicitHeight: kind === "toolbar" ? 28 : Style.controlHeight
    implicitWidth: kind === "toolbar" && !text ? 28
                 : Math.max(kind === "plain" ? 0 : 76, implicitContentWidth + leftPadding + rightPadding)
    leftPadding: kind === "toolbar" ? 6 : 14
    rightPadding: kind === "toolbar" ? 6 : 14
    topPadding: 0
    bottomPadding: 0
    font.family: Style.fontFamily
    font.pixelSize: Style.fontSize
    font.weight: filled ? Font.DemiBold : Font.Medium
    focusPolicy: Qt.TabFocus
    hoverEnabled: true
    opacity: enabled ? 1 : 0.4

    contentItem: Item {
        implicitWidth: content.implicitWidth
        implicitHeight: content.implicitHeight
        Row {
            id: content
            anchors.centerIn: parent
            spacing: 6
            Icon {
                visible: control.iconName !== ""
                name: control.iconName
                size: control.iconSize
                anchors.verticalCenter: parent.verticalCenter
                opacity: control.kind === "toolbar" ? (control.hovered || control.checked ? 0.9 : 0.6) : 0.9
            }
            Text {
                visible: control.text !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                font: control.font
                color: control.kind === "primary" ? Style.onAccent : control.filled ? "white"
                     : control.kind === "plain" ? (control.down ? Style.accentPressed : Style.accent)
                     : Style.label
                elide: Text.ElideRight
            }
        }
    }

    background: Rectangle {
        radius: control.kind === "toolbar" ? Style.radiusSmall : 7
        color: {
            switch (control.kind) {
            case "primary": return control.down ? Style.accentPressed : control.hovered ? Style.accentHover : Style.accent
            case "danger": return control.down ? Qt.darker(Style.red, 1.15) : control.hovered ? Qt.lighter(Style.red, 1.08) : Style.red
            case "plain": return "transparent"
            case "toolbar": return control.down ? Style.fillPressed : control.hovered || control.checked ? Style.fill : "transparent"
            default: return control.down ? Style.fillPressed : control.hovered ? Style.fillHover : Style.fill
            }
        }
        border.width: control.kind === "default" ? 1 : 0
        border.color: Qt.rgba(1, 1, 1, 0.06)
        Behavior on color { ColorAnimation { duration: Style.fast } }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: parent.radius + 3
            color: "transparent"
            border.width: 3
            border.color: Style.focusRing
            visible: control.visualFocus
        }
    }
}
