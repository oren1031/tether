import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Tether

// Saved commands to type into the focused pane, or into every pane in the tab
// at once, like tmux's synchronize-panes.
TSheet {
    id: dialog

    property var page: null
    property string editingId: ""
    property bool editing: false
    property string error: ""

    large: true
    width: preferredWidth(620)
    height: preferredHeight(520)
    title: editing ? (editingId ? qsTr("Edit snippet") : qsTr("New snippet")) : qsTr("Snippets")
    bottomPadding: 20

    function openFor(targetPage) {
        closeAfterSave = false
        page = targetPage
        editing = false
        error = ""
        filter.text = ""
        rebuild()
        open()
        filter.forceActiveFocus()
    }

    // From Settings: edit one snippet (or a new one), with no session to send to.
    function openEditor(s) {
        page = null
        filter.text = ""
        rebuild()
        open()
        edit(s)
        closeAfterSave = true
    }

    property bool closeAfterSave: false

    function rebuild() {
        const needle = filter.text.trim().toLowerCase()
        items.clear()
        for (const s of backend.snippets)
            if (!needle || (s.name + " " + s.text).toLowerCase().includes(needle))
                items.append({ snippet: JSON.stringify(s), name: s.name, body: s.text })
        list.currentIndex = items.count ? 0 : -1
    }

    function current() {
        return list.currentIndex >= 0 ? JSON.parse(items.get(list.currentIndex).snippet) : null
    }

    function send(all) {
        const s = current()
        if (!s || !page)
            return
        const targetPage = page
        const profile = (targetPage.activePane && targetPage.activePane.launch.profile) || {}
        const deliver = values => {
            const payload = backend.snippetPayload(s, Object.assign({ host: profile.host || "", user: profile.user || "" }, values))
            if (all)
                targetPage.broadcast(payload)
            else if (targetPage.activePane)
                targetPage.activePane.send(payload)
        }
        const names = backend.snippetVariables(s.text)
        close()
        if (!names.length) {
            deliver({})
            return
        }
        // {{placeholders}} are asked for each time, remembering the last answers.
        variables.ask(s.name, qsTr("Fill in the blanks"),
                      names.map(n => ({ key: n, label: n, text: dialog.lastValues[n] || "" })),
                      all ? qsTr("Send to all panes") : qsTr("Send"), values => {
            dialog.lastValues = Object.assign({}, dialog.lastValues, values)
            deliver(values)
        })
    }

    property var lastValues: ({})

    PromptSheet {
        id: variables
        parent: Overlay.overlay
        onClosed: if (dialog.page && dialog.page.activePane) dialog.page.activePane.focusTerminal()
    }

    function edit(s) {
        editingId = s ? s.id : ""
        name.text = s ? s.name : ""
        body.text = s ? s.text : ""
        run.checked = s ? !!s.run : true
        error = ""
        editing = true
        name.forceActiveFocus()
    }

    function save() {
        error = backend.saveSnippet({ id: editingId, name: name.text, text: body.text, run: run.checked })
        if (!error) {
            if (closeAfterSave) {
                closeAfterSave = false
                close()
                return
            }
            editing = false
            rebuild()
            filter.forceActiveFocus()
        }
    }

    onClosed: if (page && page.activePane) page.activePane.focusTerminal()

    Connections {
        target: backend
        function onSnippetsChanged() { if (!dialog.editing) dialog.rebuild() }
    }

    ListModel { id: items }

    component Caption: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.family: Style.fontFamily
        font.pixelSize: Style.smallFontSize
        color: Style.tertiaryLabel
    }

    contentItem: StackLayout {
        currentIndex: dialog.editing ? 1 : 0

        ColumnLayout {
            spacing: 10
            TField {
                id: filter
                Layout.fillWidth: true
                iconName: "search"
                placeholderText: qsTr("Search snippets")
                onTextChanged: dialog.rebuild()
                Keys.onDownPressed: list.incrementCurrentIndex()
                Keys.onUpPressed: list.decrementCurrentIndex()
                Keys.onReturnPressed: event => dialog.send(event.modifiers & Qt.ShiftModifier)
                Keys.onEnterPressed: event => dialog.send(event.modifiers & Qt.ShiftModifier)
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 10
                color: Qt.rgba(0, 0, 0, 0.18)
                border.color: Qt.rgba(1, 1, 1, 0.06)

                ListView {
                    id: list
                    anchors.fill: parent
                    anchors.margins: 5
                    clip: true
                    model: items
                    spacing: 1
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: TScrollBar {}
                    delegate: ItemDelegate {
                        id: row
                        required property int index
                        required property string name
                        required property string body
                        width: ListView.view.width
                        topPadding: 7
                        bottomPadding: 7
                        leftPadding: 10
                        rightPadding: 10
                        hoverEnabled: true
                        onClicked: list.currentIndex = index
                        onDoubleClicked: dialog.send(false)
                        background: Rectangle {
                            radius: 7
                            color: row.ListView.isCurrentItem ? Style.accent
                                 : row.hovered ? Qt.rgba(1, 1, 1, 0.04) : "transparent"
                        }
                        contentItem: ColumnLayout {
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: row.name
                                elide: Text.ElideRight
                                font.family: Style.fontFamily
                                font.pixelSize: Style.fontSize
                                font.weight: Font.Medium
                                color: row.ListView.isCurrentItem ? Style.onAccent : Style.label
                            }
                            Text {
                                Layout.fillWidth: true
                                text: row.body.replace(/\n/g, " ⏎ ")
                                elide: Text.ElideRight
                                font.family: Style.monoFamily
                                font.pixelSize: Style.smallFontSize
                                color: row.ListView.isCurrentItem ? Style.onAccentSecondary : Style.secondaryLabel
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 60
                        visible: items.count === 0
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        font.family: Style.fontFamily
                        font.pixelSize: Style.fontSize
                        color: Style.secondaryLabel
                        text: backend.snippets.length ? qsTr("No snippet matches.")
                                                      : qsTr("No snippets yet. Save a command you type often, then send it with Enter.")
                    }
                }
            }
            Caption {
                text: Style.touch ? qsTr("Send types it into the terminal; Send to all panes types it into every pane in this tab.")
                                  : qsTr("Enter sends to the focused pane. Shift+Enter sends to every pane in this tab.")
            }
            // One row on a desktop; on a phone the send buttons get their own.
            GridLayout {
                Layout.fillWidth: true
                columns: Style.compact ? 1 : 2
                columnSpacing: 8
                rowSpacing: 8
                RowLayout {
                    spacing: 2
                    TButton { kind: "toolbar"; iconName: "plus"; ToolTip.visible: hovered; ToolTip.text: qsTr("New"); onClicked: dialog.edit(null) }
                    TButton { kind: "plain"; text: qsTr("Edit"); enabled: list.currentIndex >= 0; onClicked: dialog.edit(dialog.current()) }
                    TButton {
                        kind: "plain"
                        text: qsTr("Delete")
                        enabled: list.currentIndex >= 0
                        onClicked: dialog.error = backend.removeSnippet(dialog.current().id)
                    }
                    Item { Layout.fillWidth: true }
                }
                RowLayout {
                    Layout.fillWidth: Style.compact
                    Layout.alignment: Qt.AlignRight
                    spacing: 8
                    TButton {
                        Layout.fillWidth: Style.compact
                        text: qsTr("Send to all panes")
                        enabled: list.currentIndex >= 0 && dialog.page !== null
                        onClicked: dialog.send(true)
                    }
                    TButton {
                        Layout.fillWidth: Style.compact
                        text: qsTr("Send")
                        kind: "primary"
                        enabled: list.currentIndex >= 0 && dialog.page !== null
                        onClicked: dialog.send(false)
                    }
                }
            }
        }

        ColumnLayout {
            spacing: 10
            TField {
                id: name
                Layout.fillWidth: true
                placeholderText: qsTr("Name")
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                TTextArea {
                    id: body
                    placeholderText: qsTr("Text to send. Each new line presses Enter.\n{{name}} asks for a value when sent; {{host}}, {{user}}, {{date}}, and {{time}} fill themselves in.")
                    wrapMode: TextEdit.NoWrap
                }
            }
            TCheck {
                id: run
                text: qsTr("Press Enter at the end")
            }
            Text {
                visible: dialog.error !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.family: Style.fontFamily
                font.pixelSize: Style.fontSize
                color: Style.red
                text: dialog.error
            }
            RowLayout {
                spacing: 10
                Item { Layout.fillWidth: true }
                TButton { text: qsTr("Cancel"); onClicked: {
                    if (dialog.closeAfterSave) {
                        dialog.closeAfterSave = false
                        dialog.close()
                        return
                    }
                    dialog.editing = false
                    filter.forceActiveFocus()
                } }
                TButton { text: qsTr("Save"); kind: "primary"; onClicked: dialog.save() }
            }
        }
    }
}
