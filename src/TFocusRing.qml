import QtQuick
import Tether

// The ring around the control that has keyboard focus. Put it inside the
// shape it should hug and set visible.
Rectangle {
    property real gap: 3
    anchors.fill: parent
    anchors.margins: -gap
    radius: (parent.radius || 0) + gap
    color: "transparent"
    border.width: 2
    border.color: Style.focusRing
    visible: false
}
