import QtQuick
import QtQuick.Layouts
import Tether

// A number with − and + buttons beside it, like a macOS stepper. Holding a
// button keeps stepping; with keyboard focus the arrow keys step too.
RowLayout {
    id: control
    property int value
    property int from: 0
    property int to: 100
    property string suffix: ""
    property string label: ""
    signal modified(int value)

    spacing: 8
    opacity: enabled ? 1 : 0.4

    activeFocusOnTab: true
    Accessible.role: Accessible.SpinBox
    Accessible.name: label
    Accessible.description: value + suffix
    Accessible.onIncreaseAction: step(1)
    Accessible.onDecreaseAction: step(-1)

    function step(by) {
        const next = Math.max(from, Math.min(to, value + by))
        if (next !== value)
            modified(next)
    }

    Keys.onUpPressed: step(1)
    Keys.onDownPressed: step(-1)
    Keys.onRightPressed: step(Style.mirrored ? -1 : 1)
    Keys.onLeftPressed: step(Style.mirrored ? 1 : -1)
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) {
            step(1)
            event.accepted = true
        } else if (event.key === Qt.Key_Minus) {
            step(-1)
            event.accepted = true
        } else if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
            step(event.key === Qt.Key_PageUp ? 5 : -5)
            event.accepted = true
        }
    }

    Text {
        Layout.minimumWidth: 44
        horizontalAlignment: Text.AlignRight
        text: control.value + control.suffix
        font.family: Style.fontFamily
        font.pixelSize: Style.fontSize
        font.features: { "tnum": 1 }
        color: Style.label
    }
    Rectangle {
        id: buttons
        readonly property int cell: Style.touch ? 44 : 26
        implicitWidth: cell * 2
        implicitHeight: Style.touch ? 44 : 24
        radius: 6
        color: Style.fill
        border.color: Style.fieldBorder
        TFocusRing { visible: control.activeFocus }
        Row {
            anchors.fill: parent
            // − is always on the left, as on a number line.
            LayoutMirroring.enabled: false
            Repeater {
                model: [-1, 1]
                delegate: Rectangle {
                    id: button
                    required property int modelData
                    readonly property bool usable: modelData < 0 ? control.value > control.from : control.value < control.to
                    width: buttons.cell
                    height: buttons.height
                    radius: 6
                    color: area.pressed ? Style.fillPressed : area.containsMouse ? Style.fillHover : "transparent"
                    opacity: usable ? 1 : 0.35
                    Text {
                        anchors.centerIn: parent
                        text: button.modelData < 0 ? "−" : "+"
                        font.family: Style.fontFamily
                        font.pixelSize: Style.touch ? 18 : 15
                        color: Style.label
                    }
                    MouseArea {
                        id: area
                        anchors.fill: parent
                        hoverEnabled: true
                        onPressed: {
                            control.step(button.modelData)
                            repeat.interval = 400
                            repeat.start()
                        }
                        onReleased: repeat.stop()
                        onCanceled: repeat.stop()
                        // Held down: after a pause, about twelve steps a second.
                        Timer {
                            id: repeat
                            repeat: true
                            onTriggered: {
                                interval = 80
                                if (area.pressed && button.usable)
                                    control.step(button.modelData)
                                else
                                    stop()
                            }
                        }
                    }
                }
            }
        }
        Rectangle {
            anchors.centerIn: parent
            width: 1
            height: 14
            color: Qt.rgba(1, 1, 1, 0.12)
        }
    }
}
