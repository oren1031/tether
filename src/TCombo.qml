import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Templates as T
import Tether

// A macOS pop-up button, editable or not, with a menu-like list.
ComboBox {
    id: control
    property string label: ""
    implicitHeight: Style.controlHeight + 2
    leftPadding: mirrored ? 30 : 10
    rightPadding: mirrored ? 10 : 30
    font.family: Style.fontFamily
    font.pixelSize: Style.fontSize
    hoverEnabled: true
    opacity: enabled ? 1 : 0.4

    Accessible.name: label
    Accessible.description: displayText

    delegate: ItemDelegate {
        id: item
        required property int index
        required property var modelData
        width: ListView.view.width
        height: Style.rowHeight
        leftPadding: mirrored ? 8 : 26
        rightPadding: mirrored ? 26 : 8
        hoverEnabled: true
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            text: control.textRole ? (Array.isArray(control.model) ? item.modelData[control.textRole]
                                                                   : item.modelData) : item.modelData
            font: control.font
            color: item.highlighted ? Style.accentText : Style.label
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 5
            color: item.highlighted ? Style.accent : "transparent"
            Icon {
                visible: control.currentIndex === item.index
                name: "check"
                size: 12
                color: item.highlighted ? Style.accentText : "white"
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    indicator: Rectangle {
        x: control.mirrored ? 4 : control.width - width - 4
        anchors.verticalCenter: parent.verticalCenter
        width: 22
        height: 22
        radius: 5
        color: control.editable ? "transparent" : Style.accent
        Icon {
            anchors.centerIn: parent
            name: "chevron-down"
            size: 13
            color: control.editable ? "white" : Style.accentText
            opacity: 0.95
        }
    }

    contentItem: T.TextField {
        leftPadding: 0
        rightPadding: 0
        text: control.editable ? control.editText : control.displayText
        enabled: control.editable
        autoScroll: control.editable
        readOnly: control.down
        inputMethodHints: control.inputMethodHints
        validator: control.validator
        selectByMouse: control.selectTextByMouse
        font: control.font
        color: Style.label
        selectionColor: Style.tint(0.45)
        selectedTextColor: "white"
        horizontalAlignment: Text.AlignLeft
        verticalAlignment: Text.AlignVCenter
        // The pop-up button carries the name; this is its text.
        Accessible.ignored: !control.editable
        Accessible.name: control.label
    }

    background: Rectangle {
        radius: 7
        color: control.editable ? Style.fieldFill : control.down ? Style.fillPressed : control.hovered ? Style.fillHover : Style.fill
        border.width: 1
        border.color: control.activeFocus && control.editable ? Style.focusRing : Style.fieldBorder
        TFocusRing { visible: control.activeFocus && (control.editable || control.visualFocus) }
    }

    popup: Popup {
        y: control.height + 4
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + 10, Style.touch ? 360 : 300)
        padding: 5
        topMargin: Style.safeTop
        bottomMargin: Style.safeBottom
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: TScrollBar {}
        }
        background: Rectangle {
            radius: Style.radius
            color: Style.popover
            border.color: Qt.rgba(1, 1, 1, 0.12)
        }
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Style.fast } }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 80 } }
    }
}
