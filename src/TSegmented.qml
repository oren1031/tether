import QtQuick
import QtQuick.Layouts
import Tether

// A macOS segmented control. model holds { value, text } rows; picking one
// sets value and emits activated. Tab reaches it, the arrow keys move the
// choice, and screen readers hear a group of radio buttons named label.
Rectangle {
    id: control
    property var model: []
    property var value
    property string label: ""
    signal activated(var value)

    implicitHeight: Style.controlHeight
    implicitWidth: row.implicitWidth + 4
    radius: 7
    color: Style.fieldFill
    border.color: Style.fieldBorder
    opacity: enabled ? 1 : 0.4

    activeFocusOnTab: true
    Accessible.role: Accessible.Grouping
    Accessible.name: label

    readonly property int currentIndex: {
        for (let i = 0; i < model.length; ++i)
            if (model[i].value === value)
                return i
        return -1
    }

    function pick(index) {
        if (index < 0 || index >= model.length || index === currentIndex)
            return
        value = model[index].value
        activated(model[index].value)
    }
    // Left and right follow the screen, so they swap in right-to-left languages.
    Keys.onLeftPressed: pick(currentIndex + (Style.mirrored ? 1 : -1))
    Keys.onRightPressed: pick(currentIndex + (Style.mirrored ? -1 : 1))
    Keys.onUpPressed: pick(currentIndex - 1)
    Keys.onDownPressed: pick(currentIndex + 1)

    TFocusRing { visible: control.activeFocus }

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
        anchors.left: parent.left
        anchors.leftMargin: 2
        anchors.verticalCenter: parent.verticalCenter
        Repeater {
            id: repeater
            model: control.model
            delegate: Item {
                id: segment
                required property int index
                required property var modelData
                readonly property bool chosen: control.currentIndex === index
                width: Math.max(Style.touch ? 64 : 56, label.implicitWidth + 24)
                height: control.height - 4
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.text
                Accessible.checkable: true
                Accessible.checked: chosen
                Accessible.onPressAction: control.pick(index)
                Text {
                    id: label
                    anchors.centerIn: parent
                    text: segment.modelData.text
                    font.family: Style.fontFamily
                    font.pixelSize: Style.fontSize - 1
                    font.weight: segment.chosen ? Font.DemiBold : Font.Normal
                    color: segment.chosen ? Style.label : Style.secondaryLabel
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: control.pick(segment.index)
                }
            }
        }
    }
}
