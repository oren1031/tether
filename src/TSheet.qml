import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts
import Tether

// A macOS sheet: a rounded panel over a dimmed window, with a centered bold
// title, that grows in slightly when it opens.
Dialog {
    id: sheet
    property string subtitle: ""

    modal: true
    anchors.centerIn: Overlay.overlay
    padding: 20
    topPadding: 4
    font.family: Style.fontFamily
    font.pixelSize: Style.fontSize

    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.45) }

    background: Item {
        RectangularShadow {
            anchors.fill: panel
            radius: panel.radius
            offset.y: 12
            blur: 40
            spread: 0
            color: Qt.rgba(0, 0, 0, 0.6)
        }
        Rectangle {
            id: panel
            anchors.fill: parent
            radius: Style.radiusLarge
            color: Style.sheet
            border.color: Qt.rgba(1, 1, 1, 0.10)
        }
    }

    header: ColumnLayout {
        spacing: 3
        Text {
            Layout.fillWidth: true
            Layout.topMargin: 20
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            text: sheet.title
            font.family: Style.fontFamily
            font.pixelSize: Style.titleFontSize
            font.weight: Font.Bold
            color: Style.label
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
        }
        Text {
            visible: sheet.subtitle !== ""
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            text: sheet.subtitle
            font.family: Style.fontFamily
            font.pixelSize: 12
            color: Style.secondaryLabel
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
        }
        Item { implicitHeight: 10 }
    }

    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Style.normal; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: Style.normal; easing.type: Easing.OutCubic }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 120 }
            NumberAnimation { property: "scale"; from: 1; to: 0.98; duration: 120 }
        }
    }
}
