import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Tether

// Every shortcut. Terminal programs need plain keys and most single Ctrl
// keys, so Tether's own use Ctrl+Shift, or Alt for moving between panes.
TSheet {
    id: dialog
    large: true
    width: preferredWidth(560)
    height: preferredHeight(660)
    title: qsTr("Keyboard shortcuts")
    padding: 0

    readonly property var rows: [
        ["Ctrl+Shift+K", qsTr("Search hosts and serial ports")],
        ["Enter / E / Delete", qsTr("In the host list: connect, edit, or delete")],
        ["Ctrl+Shift+T", qsTr("New tab with a local shell")],
        ["Ctrl+Shift+N", qsTr("New host")],
        ["Ctrl+Shift+E", qsTr("Split right")],
        ["Ctrl+Shift+O", qsTr("Split down")],
        ["Alt+←↑↓→", qsTr("Move to the pane in that direction")],
        ["Alt+Shift+←↑↓→", qsTr("Move the divider")],
        ["Ctrl+Shift+Z", qsTr("Zoom the pane, or show all panes again")],
        ["Ctrl+Shift+W", qsTr("Close the pane")],
        ["Ctrl+PgUp / PgDn", qsTr("Previous or next tab")],
        ["Ctrl+Shift+S", qsTr("Snippets")],
        ["Shift+Enter", qsTr("In Snippets: send to every pane in the tab")],
        ["Ctrl+Shift+F", qsTr("Files on this host, beside the pane")],
        ["F2 / Delete", qsTr("In the file list: rename or delete")],
        ["Backspace", qsTr("In the file list: the enclosing folder")],
        ["Ctrl+Shift+A", qsTr("Ask Claude for a command")],
        ["Ctrl+Shift+C / V", qsTr("Copy or paste")],
        ["Shift+PgUp / PgDn", qsTr("Scroll back through what the terminal printed")],
        ["Shift+Home / End", qsTr("Jump to the oldest or newest line")],
        ["Ctrl+= / − / 0", qsTr("Bigger, smaller, or default text")],
        ["Ctrl+Shift+B", qsTr("Show or hide the sidebar")],
        ["Ctrl+,", qsTr("Settings")],
        ["F1", qsTr("These shortcuts")],
        ["Ctrl+Shift+Q", qsTr("Quit")],
        ["Ctrl+A, Ctrl+X", qsTr("Leave a serial session (picocom)")],
    ]

    // Splits "Ctrl+Shift+K" into keycaps, keeping "/" and "," between them.
    function keys(combo) {
        if (combo.endsWith("+,"))
            return keys(combo.slice(0, -2)).concat([" , "])
        return combo.split(/(\s*[\/,]\s*|\+)/).filter(k => k && k !== "+").map(k => k.trim() || k)
    }

    contentItem: ScrollView {
        id: scroll
        clip: true
        contentWidth: availableWidth
        ScrollBar.vertical: TScrollBar { parent: scroll; x: scroll.mirrored ? 0 : scroll.width - width; height: scroll.height }

        ColumnLayout {
            x: 20
            width: scroll.availableWidth - 40
            spacing: 0

            Repeater {
                model: dialog.rows
                delegate: Rectangle {
                    id: line
                    required property int index
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 36
                    radius: 7
                    color: index % 2 === 0 ? Qt.rgba(1, 1, 1, 0.035) : "transparent"
                    Accessible.role: Accessible.ListItem
                    Accessible.name: modelData[1] + ": " + modelData[0]

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 8
                        spacing: 12
                        Text {
                            Layout.fillWidth: true
                            text: line.modelData[1]
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignLeft
                            font.family: Style.fontFamily
                            font.pixelSize: Style.fontSize
                            color: Style.label
                        }
                        Row {
                            spacing: 3
                            // Key combinations read left to right in every language.
                            LayoutMirroring.enabled: false
                            Repeater {
                                model: dialog.keys(line.modelData[0])
                                delegate: Rectangle {
                                    required property string modelData
                                    readonly property bool joiner: modelData === "/" || modelData === ","
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: joiner ? keyText.implicitWidth + 4 : Math.max(22, keyText.implicitWidth + 12)
                                    height: 22
                                    radius: 5
                                    color: joiner ? "transparent" : Qt.rgba(1, 1, 1, 0.09)
                                    border.width: joiner ? 0 : 1
                                    border.color: Qt.rgba(1, 1, 1, 0.08)
                                    Text {
                                        id: keyText
                                        anchors.centerIn: parent
                                        text: parent.modelData
                                        font.family: Style.fontFamily
                                        font.pixelSize: Style.smallFontSize + 1
                                        font.weight: Font.Medium
                                        color: parent.joiner ? Style.tertiaryLabel : Style.secondaryLabel
                                    }
                                }
                            }
                        }
                    }
                }
            }
            Item { implicitHeight: 8 }
        }
    }

    footer: Item {
        implicitHeight: 64
        TButton {
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            kind: "primary"
            text: qsTr("Done")
            onClicked: dialog.close()
        }
    }
}
