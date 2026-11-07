import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Tether

// Describe what you want to do; Claude answers with one command for the
// focused session, to insert or run. Nothing runs without a click.
TSheet {
    id: dialog
    property var page: null
    property string command: ""
    property string explanation: ""
    property bool risky: false
    property string error: ""

    width: Math.min(620, Overlay.overlay ? Overlay.overlay.width - 40 : 620)
    title: qsTr("Ask for a command")
    subtitle: {
        const p = page && page.activePane && page.activePane.launch ? page.activePane.launch.profile || {} : {}
        return p.kind === "ssh" ? qsTr("For %1").arg(p.name || p.host)
             : p.kind === "serial" ? qsTr("For the serial console %1").arg(p.name || "")
             : qsTr("For this computer")
    }

    function openFor(targetPage) {
        page = targetPage
        command = ""
        explanation = ""
        error = ""
        open()
        request.selectAll()
        request.forceActiveFocus()
    }

    function context() {
        const p = page && page.activePane && page.activePane.launch ? page.activePane.launch.profile || {} : {}
        return { kind: p.kind || "local", host: p.host || "", user: p.user || "" }
    }

    function ask() {
        error = ""
        command = ""
        assistant.ask(request.text, context())
    }

    function insert(run) {
        const pane = page && page.activePane
        const text = commandField.text
        close()
        if (pane && text)
            pane.send(run ? text + "\r" : text)
    }

    onClosed: {
        assistant.cancel()
        if (page && page.activePane)
            page.activePane.focusTerminal()
    }

    Connections {
        target: assistant
        function onAnswered(command, explanation, risky) {
            dialog.command = command
            dialog.explanation = explanation
            dialog.risky = risky
            commandField.text = command
            commandField.forceActiveFocus()
        }
        function onFailed(message) { dialog.error = message }
    }

    contentItem: ColumnLayout {
        spacing: 12

        TField {
            id: request
            Layout.fillWidth: true
            iconName: "sparkle"
            placeholderText: qsTr("Find the 10 biggest files under /var, sorted")
            enabled: assistant.keySource !== ""
            onAccepted: dialog.ask()
        }

        Text {
            visible: assistant.keySource === ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.family: Style.fontFamily
            font.pixelSize: Style.fontSize
            color: Style.secondaryLabel
            text: qsTr("Add an Anthropic API key in Settings → AI to use this. Requests go to Anthropic's API; your terminal's contents aren't sent.")
        }

        RowLayout {
            visible: assistant.busy
            spacing: 8
            Rectangle {
                width: 8
                height: 8
                radius: 4
                color: Style.accent
                SequentialAnimation on opacity {
                    running: assistant.busy
                    loops: Animation.Infinite
                    NumberAnimation { from: 1; to: 0.25; duration: 500 }
                    NumberAnimation { from: 0.25; to: 1; duration: 500 }
                }
            }
            Text {
                text: qsTr("Thinking…")
                font.family: Style.fontFamily
                font.pixelSize: Style.fontSize
                color: Style.secondaryLabel
            }
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

        // The answer: editable before it's used.
        ColumnLayout {
            visible: dialog.command !== ""
            Layout.fillWidth: true
            spacing: 8
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: commandField.implicitHeight + 16
                radius: 8
                color: Style.terminalBackground
                border.color: dialog.risky ? Style.orange : Qt.rgba(1, 1, 1, 0.12)
                TextArea {
                    id: commandField
                    anchors.fill: parent
                    anchors.margins: 4
                    wrapMode: TextEdit.Wrap
                    font.family: Style.terminalFamily
                    font.pixelSize: backend.fontSize
                    color: Style.label
                    selectByMouse: true
                    background: null
                    Keys.onReturnPressed: event => {
                        if (event.modifiers & Qt.ControlModifier)
                            dialog.insert(true)
                        else if (!(event.modifiers & Qt.ShiftModifier))
                            dialog.insert(false)
                        else
                            event.accepted = false
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Icon { visible: dialog.risky; name: "warning"; size: 13; opacity: 0.9 }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.family: Style.fontFamily
                    font.pixelSize: Style.smallFontSize + 1
                    color: dialog.risky ? Style.orange : Style.secondaryLabel
                    text: (dialog.risky ? qsTr("Careful: this changes or deletes things. ") : "") + dialog.explanation
                }
            }
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.family: Style.fontFamily
            font.pixelSize: Style.smallFontSize
            color: Style.tertiaryLabel
            text: dialog.command ? qsTr("Enter types it into the terminal; Ctrl+Enter runs it too.")
                                 : qsTr("Uses %1 through your Anthropic API key.").arg("Claude Opus 5.5")
        }
    }

    footer: RowLayout {
        spacing: 10
        Item { Layout.fillWidth: true }
        TButton { Layout.bottomMargin: 20; text: qsTr("Cancel"); onClicked: dialog.close() }
        TButton {
            Layout.bottomMargin: 20
            visible: dialog.command === ""
            Layout.rightMargin: 20
            kind: "primary"
            text: qsTr("Ask")
            enabled: !assistant.busy && request.text.trim() !== "" && assistant.keySource !== ""
            onClicked: dialog.ask()
        }
        TButton {
            Layout.bottomMargin: 20
            visible: dialog.command !== ""
            text: qsTr("Insert")
            onClicked: dialog.insert(false)
        }
        TButton {
            Layout.bottomMargin: 20
            Layout.rightMargin: 20
            visible: dialog.command !== ""
            kind: dialog.risky ? "danger" : "primary"
            text: qsTr("Run")
            onClicked: dialog.insert(true)
        }
    }
}
