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
    // Big sheets (settings, editors) fill a phone's screen instead of floating.
    property bool large: false
    readonly property bool fills: large && Style.compact

    // The room for a sheet: the window, less the system's bars and the
    // on-screen keyboard where they reach into it (phones).
    readonly property real roomWidth: (Overlay.overlay ? Overlay.overlay.width : 0) - Style.safeLeft - Style.safeRight
    readonly property real roomHeight: (Overlay.overlay ? Overlay.overlay.height : 0) - Style.safeTop - Style.safeBottom

    function preferredWidth(most) {
        if (!Overlay.overlay)
            return most
        return fills ? roomWidth : Math.min(most, roomWidth - 40)
    }
    function preferredHeight(most) {
        if (!Overlay.overlay)
            return most
        return fills ? roomHeight : Math.min(most, roomHeight - 40)
    }

    modal: true
    // Escape (Back on a phone) and its own buttons close a sheet; a stray
    // click on the dimmed window doesn't, so a half-filled form isn't lost.
    closePolicy: Popup.CloseOnEscape
    // Centered in that room, so buttons never sit under the navigation bar
    // and a sheet with fields moves up above the keyboard.
    parent: Overlay.overlay
    x: Style.safeLeft + Math.round((roomWidth - width) / 2)
    y: Style.safeTop + Math.round((roomHeight - height) / 2)
    height: Math.min(implicitHeight, Math.max(160, roomHeight - 16))
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
            // A sheet that fills the screen colors the strips under the bars too.
            anchors.topMargin: sheet.fills ? -Style.safeTop : 0
            anchors.bottomMargin: sheet.fills ? -Style.safeBottom : 0
            anchors.leftMargin: sheet.fills ? -Style.safeLeft : 0
            anchors.rightMargin: sheet.fills ? -Style.safeRight : 0
            radius: sheet.fills ? 0 : Style.radiusLarge
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
