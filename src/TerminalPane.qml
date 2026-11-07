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

    Timer {
        id: startupWait
        property int tries: 0
        interval: 700
        repeat: true
        onTriggered: {
            const profile = pane.launch.profile || {}
            if (pane.ended || ++tries > 130) {
                stop()
                return
            }
            if (profile.kind === "ssh" && !backend.connectionReady(profile))
                return
            stop()
            startupSettle.interval = profile.kind === "ssh" && backend.settings.shareConnections ? 900 : 2500
            startupSettle.start()
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
            font.family: Style.terminalFamily
            font.pixelSize: backend.fontSize
            colors: Style.terminalColors
            lineSpacing: backend.settings.lineSpacing
            blinkingCursor: backend.settings.blinkingCursor
            boldText: backend.settings.boldText

            onFinished: code => {
                pane.ended = true
                pane.lostConnection = code === 255 && !!pane.launch.reconnect
                if (pane.lostConnection) {
                    pane.reconnectIn = 3
                    reconnectTimer.start()
                }
            }
            onActiveFocusChanged: if (activeFocus && pane.page) pane.page.activePane = pane

            // A thin scroll bar for the history, shown while there is some.
            Rectangle {
                visible: term.scrollbackLines > 0
                anchors.right: parent.right
                width: scrollArea.containsMouse || scrollArea.pressed ? 8 : 5
                radius: width / 2
                color: Qt.rgba(1, 1, 1, scrollArea.pressed ? 0.45 : 0.28)
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
        height: 44
        radius: 12
        color: Style.popover
        border.color: activeFocus ? Style.tint(0.8) : Qt.rgba(1, 1, 1, 0.12)
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
                text: pane.lostConnection && reconnectTimer.running
                      ? qsTr("Connection lost. Reconnecting in %n seconds…", "", pane.reconnectIn)
                      : pane.lostConnection ? qsTr("Connection lost. Press Enter to reconnect, or Ctrl+Shift+W to close the pane.")
                      : qsTr("Session ended. Press Enter to reconnect, or Ctrl+Shift+W to close the pane.")
            }
            TButton {
                kind: "primary"
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
        height: 44
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
                        { label: "Esc", key: "esc" }, { label: "Tab", key: "tab" },
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
                        height: 36
                        radius: 7
                        color: on ? Style.accent : keyArea.pressed ? Style.fillPressed : Style.fill
                        Text {
                            id: keyLabel
                            anchors.centerIn: parent
                            text: parent.modelData.label
                            font.family: Style.fontFamily
                            font.pixelSize: Style.fontSize
                            font.weight: Font.Medium
                            color: parent.on ? Style.onAccent : Style.label
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
}
