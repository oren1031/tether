import QtQuick
import QtQuick.Layouts
import Tether

// A macOS segmented control. model holds { value, text } rows; picking one
// sets value and emits activated.
Rectangle {
    id: control
    property var model: []
    property var value
    signal activated(var value)

    implicitHeight: Style.controlHeight
    implicitWidth: row.implicitWidth + 4
    radius: 7
    color: Style.fieldFill
    border.color: Style.fieldBorder

    readonly property int currentIndex: {
        for (let i = 0; i < model.length; ++i)
            if (model[i].value === value)
                return i
        return -1
    }

    // The raised thumb under the chosen segment.
    Rectangle {
        visible: control.currentIndex >= 0
        // Reading count re-evaluates this once the segments exist.
        readonly property Item target: repeater.count > 0 && control.currentIndex >= 0
                                       ? repeater.itemAt(control.currentIndex) : null
        x: target ? row.x + target.x : 0
        y: 2
        width: target ? target.width : 0
        height: parent.height - 4
        radius: 5
        color: Qt.rgba(1, 1, 1, 0.16)
        border.color: Qt.rgba(1, 1, 1, 0.08)
        Behavior on x { NumberAnimation { duration: Style.normal; easing.type: Easing.OutCubic } }
        Behavior on width { NumberAnimation { duration: Style.normal; easing.type: Easing.OutCubic } }
    }

    Row {
        id: row
        x: 2
        anchors.verticalCenter: parent.verticalCenter
        Repeater {
            id: repeater
            model: control.model
            delegate: Item {
                required property int index
                required property var modelData
                width: Math.max(56, label.implicitWidth + 24)
                height: control.height - 4
                Text {
                    id: label
                    anchors.centerIn: parent
                    text: parent.modelData.text
                    font.family: Style.fontFamily
                    font.pixelSize: Style.fontSize - 1
                    font.weight: control.currentIndex === parent.index ? Font.DemiBold : Font.Normal
                    color: control.currentIndex === parent.index ? Style.label : Style.secondaryLabel
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        control.value = parent.modelData.value
                        control.activated(parent.modelData.value)
                    }
                }
            }
        }
    }
}
