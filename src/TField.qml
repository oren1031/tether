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

    // On a phone the keyboard covers the lower half of a form: the field
    // being typed in scrolls into view, when it's tapped and when the
    // keyboard comes up.
    function reveal() {
        if (!activeFocus)
            return
        let view = parent
        while (view && !(view instanceof Flickable))
            view = view.parent
        if (!view)
            return
        const top = mapToItem(view.contentItem, 0, 0).y - 16
        const bottom = top + height + 32
        if (top < view.contentY)
            view.contentY = Math.max(0, top)
        else if (bottom > view.contentY + view.height)
            view.contentY = bottom - view.height
    }
    Timer { id: revealTimer; interval: 120; onTriggered: control.reveal() }
    onActiveFocusChanged: if (activeFocus && Style.touch) revealTimer.restart()
    Connections {
        target: Style
        enabled: control.activeFocus
        function onSafeBottomChanged() { revealTimer.restart() }
    }

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
