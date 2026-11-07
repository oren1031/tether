import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Tether
import QMLTermWidget

// One terminal session. The terminal sits in a Loader so reconnecting starts
// a fresh session in the same place in the split layout.
FocusScope {
    id: pane

    readonly property bool isPane: true
    property var container: null
    property var page: null
    property var launch: ({})
    property bool ended: false
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
            loader.item.session.sendText(text)
    }

    function restart(newLaunch) {
        ended = false
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
        anchors.bottomMargin: 4

        sourceComponent: QMLTermWidget {
            id: term
            font.family: Style.terminalFamily
            font.pixelSize: backend.fontSize
            colorScheme: Style.terminalScheme
            lineSpacing: backend.settings.lineSpacing
            blinkingCursor: backend.settings.blinkingCursor
            enableBold: backend.settings.boldText

            session: QMLTermSession {
                id: session
                initialWorkingDirectory: pane.launch.workingDirectory || ""
                shellProgram: pane.launch.program || ""
                shellProgramArgs: pane.launch.args || []
                onFinished: pane.ended = true
            }

            onActiveFocusChanged: if (activeFocus && pane.page) pane.page.activePane = pane

            // Clicking a pane focuses it without taking the click from the
            // terminal, which still selects text.
            TapHandler {
                acceptedButtons: Qt.AllButtons
                onPressedChanged: if (pressed) term.forceActiveFocus()
            }

            QMLTermScrollbar {
                terminal: term
                width: 5
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: Qt.rgba(1, 1, 1, 0.3)
                }
            }

            Component.onCompleted: {
                session.startShellProgram()
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
                text: qsTr("Session ended. Press Enter to reconnect, or Ctrl+Shift+W to close the pane.")
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
}
