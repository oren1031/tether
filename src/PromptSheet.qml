import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Tether

// Asks for one or more values: a new name, a folder name, or a snippet's
// {{placeholders}}. ask() takes the fields and what to do with the answers.
TSheet {
    id: sheet
    property var fields: []
    property string acceptLabel: qsTr("OK")
    property var onAnswered: null

    width: Math.min(420, Overlay.overlay ? Overlay.overlay.width - 40 : 420)

    // fields: [{ key, label, text }]; done(values) gets { key: text }.
    function ask(heading, subtitleText, fieldList, label, done) {
        title = heading
        subtitle = subtitleText || ""
        acceptLabel = label || qsTr("OK")
        onAnswered = done
        fields = fieldList
        open()
        Qt.callLater(() => { if (repeater.count) repeater.itemAt(0).field.forceActiveFocus() })
    }

    function accept_() {
        const values = {}
        for (let i = 0; i < repeater.count; ++i)
            values[fields[i].key] = repeater.itemAt(i).field.text
        const done = onAnswered
        close()
        if (done)
            done(values)
    }

    contentItem: ColumnLayout {
        spacing: 10
        Repeater {
            id: repeater
            model: sheet.fields
            delegate: ColumnLayout {
                required property int index
                required property var modelData
                readonly property alias field: input
                Layout.fillWidth: true
                spacing: 4
                Text {
                    visible: sheet.fields.length > 1 || modelData.label
                    text: modelData.label || modelData.key
                    font.family: Style.fontFamily
                    font.pixelSize: Style.smallFontSize
                    color: Style.secondaryLabel
                }
                TField {
                    id: input
                    Layout.fillWidth: true
                    text: modelData.text || ""
                    onAccepted: {
                        if (index + 1 < repeater.count)
                            repeater.itemAt(index + 1).field.forceActiveFocus()
                        else
                            sheet.accept_()
                    }
                    Component.onCompleted: selectAll()
                }
            }
        }
    }

    footer: RowLayout {
        spacing: 10
        Item { Layout.fillWidth: true }
        TButton { Layout.bottomMargin: 20; text: qsTr("Cancel"); onClicked: sheet.close() }
        TButton { Layout.bottomMargin: 20; Layout.rightMargin: 20; kind: "primary"; text: sheet.acceptLabel; onClicked: sheet.accept_() }
    }
}
