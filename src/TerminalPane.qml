import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Tether

// One terminal session. The terminal sits in a Loader so reconnecting starts
// a fresh session in the same place in the split layout.
FocusScope {
    id: pane

    readonly property bool isPane: true
    property var container: null
    property var page: null
    property var launch: ({})
    property bool ended: false
    // A dropped ssh connection (exit 255) reconnects on its own, counting down.
    property int reconnectIn: 0
    property bool lostConnection: false
    // ssh refused the server's key: reconnecting can't help until it's dealt with.
    property bool hostKeyFailed: false
    property bool hostKeyChanged: false
    readonly property string title: launch.title || ""
    readonly property bool active: page !== null && page.activePane === pane
    readonly property var terminal: loader.item

    signal reconnectRequested()

    function focusTerminal() {
        if (page)
            page.activePane = pane
        if (ended)
            endBar.forceActiveFocus()
        else if (loader.item)
            loader.item.forceActiveFocus()
    }

    function send(text) {
        if (loader.item && !ended)
            loader.item.sendText(text)
    }

    function restart(newLaunch) {
        ended = false
        lostConnection = false
        hostKeyFailed = false
        hostKeyChanged = false
        reconnectTimer.stop()
        launch = newLaunch
        loader.active = false
        loader.active = true
        Qt.callLater(focusTerminal)
    }

    readonly property bool split: page !== null && page.paneCount > 1

    // A host's startup snippets are typed once its login is done: for ssh,
    // when the shared connection is up, so nothing lands in a password prompt.
    function beginStartup() {
        const ids = (launch.profile || {}).startupSnippets || []
        if (!ids.length)
            return
        startupWait.tries = 0
        startupWait.start()
    }

    function sendStartup() {
        const profile = launch.profile || {}
        for (const id of profile.startupSnippets || [])
            for (const s of backend.snippets)
                if (s.id === id)
                    send(backend.snippetPayload(s, { host: profile.host || "", user: profile.user || "" }))
    }

    // The backend's answers carry this, so each pane hears only its own.
    readonly property string token: "pane-" + Math.random().toString(36).slice(2)

    Timer {
        id: startupWait
        property int tries: 0
        interval: 700
        repeat: true
        function ready() {
            const profile = pane.launch.profile || {}
            stop()
            startupSettle.interval = profile.kind === "ssh" && backend.settings.shareConnections ? 900 : 2500
            startupSettle.start()
        }
        onTriggered: {
            const profile = pane.launch.profile || {}
            if (pane.ended || ++tries > 130) {
                stop()
                return
            }
            // ssh is asked without waiting for it; onConnectionChecked hears.
            if (profile.kind === "ssh")
                backend.checkConnection(profile, pane.token)
            else
                ready()
        }
    }
    Connections {
        target: backend
        function onConnectionChecked(token, ready) {
            if (token === pane.token && ready && startupWait.running)
                startupWait.ready()
        }
        function onHostKeyForgotten(token, error) {
            if (token !== pane.token)
                return
            if (error)
                forgetError.text = error
            else
                pane.reconnectRequested()
        }
    }
    Timer {
        id: startupSettle
        onTriggered: if (!pane.ended) pane.sendStartup()
    }

    onEndedChanged: if (ended && active) endBar.forceActiveFocus()

    Rectangle {
        anchors.fill: parent
        color: Style.terminalBackground
    }

    Loader {
        id: loader
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 4
        anchors.topMargin: 8
        anchors.bottomMargin: touchKeys.visible ? touchKeys.height + 4 : 4

        sourceComponent: TerminalView {
            id: term
            Accessible.role: Accessible.Terminal
            Accessible.name: qsTr("Terminal")
            font.family: Style.terminalFamily
            font.pixelSize: backend.fontSize
            colors: Style.terminalColors
            lineSpacing: backend.settings.lineSpacing
            blinkingCursor: backend.settings.blinkingCursor
            boldText: backend.settings.boldText

            onFinished: code => {
                pane.ended = true
                const screen = screenText(40)
                pane.hostKeyChanged = screen.indexOf("REMOTE HOST IDENTIFICATION HAS CHANGED") >= 0
                pane.hostKeyFailed = pane.hostKeyChanged || screen.indexOf("Host key verification failed") >= 0
                pane.lostConnection = code === 255 && !!pane.launch.reconnect && !pane.hostKeyFailed
                if (pane.lostConnection) {
                    pane.reconnectIn = 3
                    reconnectTimer.start()
                }
            }
            onActiveFocusChanged: if (activeFocus && pane.page) pane.page.activePane = pane
            confirmPaste: backend.settings.confirmPaste
            onHostKeyQuestion: (host, keyType, fingerprint, changed, oldFingerprints) =>
                trustSheet.ask(host, keyType, fingerprint, changed, oldFingerprints)
            onPasteQuestion: (text, lines) => pasteSheet.ask(text, lines)

            // A thin scroll bar for the history, shown while there is some.
            Rectangle {
                visible: term.scrollbackLines > 0
                anchors.right: parent.right
                width: scrollArea.containsMouse || scrollArea.pressed ? 8 : 5
                radius: width / 2
                // In the terminal's text color, so it shows on a light scheme too.
                color: Qt.rgba(Style.terminalForeground.r, Style.terminalForeground.g, Style.terminalForeground.b,
                               scrollArea.pressed ? 0.6 : 0.45)
                opacity: term.scrollOffset > 0 || scrollArea.containsMouse || scrollArea.pressed ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 200 } }
                readonly property real total: term.scrollbackLines + term.rows
                height: Math.max(24, term.height * term.rows / total)
                y: (term.height - height) * (term.scrollbackLines - term.scrollOffset) / Math.max(1, term.scrollbackLines)
            }
            MouseArea {
                id: scrollArea
                anchors.right: parent.right
                width: 12
                height: parent.height
                hoverEnabled: true
                visible: term.scrollbackLines > 0
                function scrollTo(y) {
                    const fraction = Math.max(0, Math.min(1, y / height))
                    term.scrollOffset = Math.round(term.scrollbackLines * (1 - fraction))
                }
                onPressed: mouse => scrollTo(mouse.y)
                onPositionChanged: mouse => { if (pressed) scrollTo(mouse.y) }
            }

            Component.onCompleted: {
                logFile = pane.launch.logPath || ""
                if (pane.launch.serial)
                    startSerial(pane.launch.serial)
                else if (pane.launch.ssh)
                    startSsh(pane.launch.ssh)
                else
                    start(pane.launch.program || "", pane.launch.args || [], pane.launch.workingDirectory || "",
                          pane.launch.environment || [])
                pane.beginStartup()
            }
        }
    }

    // Once a tab is split, the panes not in focus dim a little and the one
    // in focus gets a soft blue outline.
    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: pane.split && !pane.active && backend.settings.paneDimming ? 0.22 : 0
        Behavior on opacity { NumberAnimation { duration: Style.normal } }
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: 1
        color: "transparent"
        radius: 6
        border.width: 1
        border.color: Style.tint(0.7)
        opacity: pane.split && pane.active ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Style.normal } }
    }

    // When the session ends, a floating bar offers to start it again.
    Rectangle {
        id: endBar
        visible: pane.ended
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        width: Math.min(parent.width - 32, endRow.implicitWidth + 28)
        height: forgetError.text ? 64 : 44
        radius: 12
        color: Style.popover
        Text {
            id: forgetError
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 6
            anchors.horizontalCenter: parent.horizontalCenter
            font.family: Style.fontFamily
            font.pixelSize: Style.smallFontSize
            color: Style.errorText
        }
        border.color: activeFocus ? Style.focusRing : Qt.rgba(1, 1, 1, 0.12)
        activeFocusOnTab: true

        Keys.onReturnPressed: pane.reconnectRequested()
        Keys.onEscapePressed: reconnectTimer.stop()
        Keys.onEnterPressed: pane.reconnectRequested()

        RowLayout {
            id: endRow
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 8
            spacing: 12
            Text {
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.family: Style.fontFamily
                font.pixelSize: 12
                color: Style.secondaryLabel
                text: pane.hostKeyChanged ? qsTr("This server's key has changed. If you know it was reinstalled, forget the old key and connect again.")
                      : pane.hostKeyFailed ? qsTr("The server's key wasn't trusted. Reconnect to be asked again.")
                      : pane.lostConnection && reconnectTimer.running
                      ? qsTr("Connection lost. Reconnecting in %n seconds…", "", pane.reconnectIn)
                      : pane.lostConnection ? qsTr("Connection lost. Press Enter to reconnect, or Ctrl+Shift+W to close the pane.")
                      : qsTr("Session ended. Press Enter to reconnect, or Ctrl+Shift+W to close the pane.")
            }
            TButton {
                visible: pane.hostKeyChanged
                kind: "danger"
                text: qsTr("Forget old key")
                onClicked: {
                    forgetError.text = ""
                    backend.forgetHostKey(pane.launch.profile || {}, pane.token)
                }
            }
            TButton {
                kind: pane.hostKeyChanged ? "default" : "primary"
                iconName: "reload"
                iconSize: 13
                text: qsTr("Reconnect")
                onClicked: pane.reconnectRequested()
            }
        }
    }

    Timer {
        id: reconnectTimer
        interval: 1000
        repeat: true
        onTriggered: {
            if (!pane.ended) {
                stop()
                return
            }
            if (--pane.reconnectIn <= 0) {
                stop()
                pane.reconnectRequested()
            }
        }
    }
    onReconnectRequested: reconnectTimer.stop()

    // Keys a phone keyboard lacks, under the focused terminal on touch screens.
    Rectangle {
        id: touchKeys
        visible: (Qt.platform.os === "android" || Qt.platform.os === "ios") && pane.active && !pane.ended
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 52
        color: Style.toolbar
        Rectangle { width: parent.width; height: 1; color: Style.separator }
        Flickable {
            anchors.fill: parent
            anchors.topMargin: 1
            contentWidth: keyRow.width
            clip: true
            flickableDirection: Flickable.HorizontalFlick
            Row {
                id: keyRow
                height: parent.height
                padding: 4
                spacing: 4
                Repeater {
                    model: [
                        { label: "Esc", key: "esc" }, { label: "⏎", key: "enter" }, { label: "Tab", key: "tab" },
                        { label: "Ctrl", sticky: "ctrl" }, { label: "Alt", sticky: "alt" },
                        { label: "←", key: "left" }, { label: "↓", key: "down" }, { label: "↑", key: "up" },
                        { label: "→", key: "right" }, { label: "|", text: "|" }, { label: "/", text: "/" },
                        { label: "-", text: "-" }, { label: "~", text: "~" }, { label: "Home", key: "home" },
                        { label: "End", key: "end" }, { label: "PgUp", key: "pgup" }, { label: "PgDn", key: "pgdn" },
                        { label: "Paste", paste: true }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool on: pane.terminal !== null
                            && ((modelData.sticky === "ctrl" && pane.terminal.stickyCtrl)
                                || (modelData.sticky === "alt" && pane.terminal.stickyAlt))
                        width: Math.max(44, keyLabel.implicitWidth + 20)
                        height: 44
                        radius: 7
                        color: on ? Style.accent : keyArea.pressed ? Style.fillPressed : Style.fill
                        Accessible.role: Accessible.Button
                        Accessible.name: modelData.label
                        Accessible.checkable: !!modelData.sticky
                        Accessible.checked: on
                        Text {
                            id: keyLabel
                            anchors.centerIn: parent
                            text: parent.modelData.label
                            font.family: Style.fontFamily
                            font.pixelSize: Style.fontSize
                            font.weight: Font.Medium
                            color: parent.on ? Style.accentText : Style.label
                        }
                        MouseArea {
                            id: keyArea
                            anchors.fill: parent
                            onClicked: {
                                const t = pane.terminal
                                if (!t)
                                    return
                                const m = parent.modelData
                                if (m.sticky === "ctrl")
                                    t.stickyCtrl = !t.stickyCtrl
                                else if (m.sticky === "alt")
                                    t.stickyAlt = !t.stickyAlt
                                else if (m.key)
                                    t.sendKey(m.key)
                                else if (m.text)
                                    t.sendText(m.text)
                                else if (m.paste)
                                    t.pasteClipboard()
                                t.forceActiveFocus()
                            }
                        }
                    }
                }
            }
        }
    }

    // The built-in ssh's host key question as a dialog, which works with any
    // keyboard; typing yes in the terminal works too.
    TSheet {
        id: trustSheet
        parent: Overlay.overlay
        property string host
        property string keyType
        property string fingerprint
        property bool changed: false
        property var oldFingerprints: []
        property bool answered: false
        // A changed key is only trusted with the host's name typed out.
        readonly property bool confirmed: !changed || typedHost.text.trim().toLowerCase() === host.toLowerCase()
        width: Math.min(460, Overlay.overlay ? Overlay.overlay.width - 32 : 460)
        title: changed ? qsTr("%1's key has changed").arg(host) : qsTr("Trust %1?").arg(host)
        closePolicy: Popup.NoAutoClose

        function ask(h, type, print, isChanged, oldPrints) {
            host = h
            keyType = type
            fingerprint = print
            changed = isChanged
            oldFingerprints = oldPrints || []
            typedHost.text = ""
            answered = false
            open()
        }
        function answer(trust) {
            answered = true
            if (pane.terminal)
                pane.terminal.answerHostKey(trust)
            close()
        }
        onClosed: if (!answered && pane.terminal) pane.terminal.answerHostKey(false)
        onOpened: trustKeys.forceActiveFocus()

        contentItem: ColumnLayout {
            spacing: 10
            // For a first connection Enter trusts and Escape declines, from
            // any keyboard. A changed key has no shortcut to yes: Enter and
            // Escape both decline.
            Item {
                id: trustKeys
                focus: true
                Keys.onReturnPressed: trustSheet.answer(!trustSheet.changed)
                Keys.onEnterPressed: trustSheet.answer(!trustSheet.changed)
                Keys.onEscapePressed: trustSheet.answer(false)
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.family: Style.fontFamily
                font.pixelSize: Style.fontSize
                color: trustSheet.changed ? Style.orange : Style.secondaryLabel
                text: trustSheet.changed
                      ? qsTr("The server answered with a different key than last time. That happens when a server is reinstalled, but it can also mean someone is intercepting the connection. Don't connect unless you expected this.")
                      : qsTr("This is the first connection to this server. Check that the fingerprint matches the server's, then trust it; Tether remembers it from now on.")
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: printText.implicitHeight + 20
                radius: 8
                color: Qt.rgba(0, 0, 0, 0.25)
                Text {
                    id: printText
                    anchors.fill: parent
                    anchors.margins: 10
                    wrapMode: Text.WrapAnywhere
                    font.family: Style.monoFamily
                    font.pixelSize: Style.smallFontSize + 1
                    color: Style.label
                    text: !trustSheet.changed ? trustSheet.keyType + "\n" + trustSheet.fingerprint
                          : qsTr("Saved key") + "\n" + (trustSheet.oldFingerprints.length ? trustSheet.oldFingerprints.join("\n") : "?")
                            + "\n\n" + qsTr("New key") + "\n" + trustSheet.keyType + " " + trustSheet.fingerprint
                }
            }
            Text {
                visible: trustSheet.changed
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.family: Style.fontFamily
                font.pixelSize: Style.fontSize
                color: Style.secondaryLabel
                text: qsTr("To replace the saved key, type the host's name (%1).").arg(trustSheet.host)
            }
            TField {
                id: typedHost
                visible: trustSheet.changed
                Layout.fillWidth: true
                placeholderText: trustSheet.host
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                Keys.onEscapePressed: trustSheet.answer(false)
            }
        }
        footer: RowLayout {
            spacing: 10
            Item { Layout.fillWidth: true }
            TButton {
                Layout.bottomMargin: 20
                kind: trustSheet.changed ? "primary" : "default"
                text: qsTr("Don't connect")
                onClicked: trustSheet.answer(false)
            }
            TButton {
                Layout.bottomMargin: 20
                Layout.rightMargin: 20
                kind: trustSheet.changed ? "danger" : "primary"
                enabled: trustSheet.confirmed
                text: trustSheet.changed ? qsTr("Trust the new key") : qsTr("Trust and connect")
                onClicked: if (trustSheet.confirmed) trustSheet.answer(true)
            }
        }
    }

    // A paste that would run several lines at once is shown first.
    TSheet {
        id: pasteSheet
        parent: Overlay.overlay
        property string text
        property int lines: 0
        width: Math.min(520, Overlay.overlay ? Overlay.overlay.width - 32 : 520)
        title: qsTr("Paste %n lines?", "", lines)
        subtitle: qsTr("This program runs each line as it arrives.")

        function ask(pasted, count) {
            text = pasted
            lines = count
            open()
            pasteCancel.forceActiveFocus()
        }
        function paste() {
            const pasted = text
            close()
            if (pane.terminal)
                pane.terminal.paste(pasted)
        }
        onClosed: {
            text = ""
            pane.focusTerminal()
        }

        contentItem: Rectangle {
            implicitHeight: Math.min(220, pastePreview.implicitHeight + 20)
            radius: 8
            color: Qt.rgba(0, 0, 0, 0.25)
            clip: true
            Text {
                id: pastePreview
                anchors.fill: parent
                anchors.margins: 10
                wrapMode: Text.WrapAnywhere
                elide: Text.ElideRight
                textFormat: Text.PlainText
                font.family: Style.monoFamily
                font.pixelSize: Style.smallFontSize + 1
                color: Style.label
                text: pasteSheet.text.replace(/\r/g, "\n")
            }
        }
        footer: RowLayout {
            spacing: 10
            Item { Layout.fillWidth: true }
            TButton { id: pasteCancel; Layout.bottomMargin: 20; text: qsTr("Cancel"); onClicked: pasteSheet.close() }
            TButton {
                Layout.bottomMargin: 20
                Layout.rightMargin: 20
                kind: "primary"
                text: qsTr("Paste")
                onClicked: pasteSheet.paste()
            }
        }
    }
}
