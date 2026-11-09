import QtQuick
import QtQuick.Controls.Basic
import Tether

// A macOS push button. kind is "default" (gray), "primary" (blue), "plain"
// (text only), "danger" (red), or "toolbar" (an icon that lights up on hover).
// label names a button that shows only an icon: screen readers read it, and
// it's the tool tip (on a touch screen, after a long press).
Button {
    id: control
    property string kind: highlighted ? "primary" : flat ? "plain" : "default"
    property string iconName: ""
    property int iconSize: 15
    property string label: ""

    readonly property bool filled: kind === "primary" || kind === "danger"
    // What's written on a filled button: picked to read on its color.
    readonly property color filledText: kind === "primary" ? Style.accentText : Style.textOn(Style.red)

    implicitHeight: kind === "toolbar" ? Style.toolbarButton : Style.controlHeight
    implicitWidth: kind === "toolbar" && !text ? Style.toolbarButton
                 : Math.max(kind === "plain" ? (Style.touch ? 44 : 0) : 76, implicitContentWidth + leftPadding + rightPadding)
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

    Accessible.role: Accessible.Button
    Accessible.name: text || label
    Accessible.description: text && label ? label : ""

    ToolTip.visible: label !== "" && (hovered || (Style.touch && pressed))
    ToolTip.delay: 600
    ToolTip.text: label

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
                color: control.filled ? control.filledText : "white"
                anchors.verticalCenter: parent.verticalCenter
                opacity: control.kind === "toolbar" ? (control.hovered || control.checked || control.visualFocus ? 0.9 : 0.6) : 0.9
            }
            Text {
                visible: control.text !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                font: control.font
                color: control.filled ? control.filledText
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

        TFocusRing { visible: control.visualFocus }
    }
}
