import QtQuick
import QtQuick.Layouts
import Tether

// A number with − and + buttons beside it, like a macOS stepper.
RowLayout {
    id: control
    property int value
    property int from: 0
    property int to: 100
    property string suffix: ""
    signal modified(int value)

    spacing: 8

    function step(by) {
        const next = Math.max(from, Math.min(to, value + by))
        if (next !== value)
            modified(next)
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
        implicitWidth: 52
        implicitHeight: 24
        radius: 6
        color: Style.fill
        border.color: Qt.rgba(1, 1, 1, 0.06)
        Row {
            anchors.fill: parent
            Repeater {
                model: [-1, 1]
                delegate: Rectangle {
                    required property int modelData
                    width: 26
                    height: 24
                    radius: 6
                    color: area.pressed ? Style.fillPressed : area.containsMouse ? Style.fillHover : "transparent"
                    opacity: (modelData < 0 ? control.value > control.from : control.value < control.to) ? 1 : 0.35
                    Text {
                        anchors.centerIn: parent
                        text: parent.modelData < 0 ? "−" : "+"
                        font.family: Style.fontFamily
                        font.pixelSize: 15
                        color: Style.label
                    }
                    MouseArea {
                        id: area
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: control.step(parent.modelData)
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
