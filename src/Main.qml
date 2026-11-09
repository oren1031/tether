import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Tether

ApplicationWindow {
    id: win
    width: 1200
    height: 760
    minimumWidth: 720
    minimumHeight: 460
    visible: true
    title: currentPage && currentPage.title ? qsTr("%1 · Tether").arg(currentPage.title) : "Tether"
    color: Style.window
    font.family: Style.fontFamily
    font.pixelSize: Style.fontSize

    // For the few stock controls left, like tool tips.
    palette.window: Style.sheet
    palette.windowText: Style.label
    palette.base: Style.sheet
    palette.text: Style.label
    palette.button: Style.popover
    palette.buttonText: Style.label
    palette.toolTipBase: Style.popover
    palette.toolTipText: Style.label
    palette.highlight: Style.accent
    palette.highlightedText: Style.onAccent

    // Phones and narrow windows: the sidebar takes the whole width while it's
    // open and steps aside once a session starts.
    readonly property bool narrow: width < 720
    readonly property bool touch: backend.touchUi
    Binding { target: Style; property: "compact"; value: win.width < 600 }
    // Phones draw under the status and navigation bars, and the keyboard
    // covers the bottom of the window. The window's own content is kept
    // clear of them by Qt; sheets and menus use these.
    //
    // Qt counts the keyboard in the bottom margin most of the time but loses
    // it when the keyboard changes its mode (a password prompt ending), so
    // how much of the window it covers is measured here as well. Qt for
    // Android gives the keyboard's rectangle in screen pixels.
    readonly property real keyboardCover: {
        const r = Qt.inputMethod.keyboardRectangle
        if (!touch || !Qt.inputMethod.visible || r.height <= 0)
            return 0
        const scale = r.y + r.height > Screen.height * 1.2 ? Screen.devicePixelRatio : 1
        return Math.max(0, Math.min(height, y + height - r.y / scale))
    }
    readonly property real safeBottom: Math.max(SafeArea.margins.bottom, keyboardCover)
    bottomPadding: safeBottom
    Binding { target: Style; property: "safeTop"; value: win.SafeArea.margins.top }
    Binding { target: Style; property: "safeBottom"; value: win.safeBottom }
    Binding { target: Style; property: "safeLeft"; value: win.SafeArea.margins.left }
    Binding { target: Style; property: "safeRight"; value: win.SafeArea.margins.right }
    property bool sidebarVisible: true
    onNarrowChanged: if (narrow && tabModel.count > 0) sidebarVisible = false
    property bool quitting: false
    readonly property var currentPage: tabs.currentIndex >= 0 && tabs.currentIndex < pages.count
                                       ? pages.itemAt(tabs.currentIndex) : null
    readonly property bool dialogOpen: hostDialog.visible || snippetsDialog.visible || shortcutsDialog.visible
                                       || message.visible || settingsDialog.visible || askDialog.visible
    readonly property bool activeIsSsh: currentPage !== null && currentPage.activePane !== null
                                        && !!currentPage.activePane.launch.profile
                                        && currentPage.activePane.launch.profile.kind === "ssh"

    property bool updateInstalling: false

    function installUpdate(command) {
        if (updater.installKind === "appimage") {
            if (!updater.replaceAppImage())
                showMessage(qsTr("Update"), updater.message || qsTr("The update couldn't be installed."))
            return
        }
        if (updater.installKind === "android") {
            if (!updater.launchInstaller())
                showMessage(qsTr("Update"), qsTr("Android's installer couldn't open. Get the update from the release page."))
            return
        }
        if (updater.installKind === "windows") {
            if (!updater.launchInstaller())
                showMessage(qsTr("Update"), qsTr("The installer couldn't start. Get it from the release page."))
            return
        }
        // pacman runs in a tab, where sudo can ask for the password.
        const launch = { program: "/usr/bin/sudo", args: command.args, title: qsTr("Installing update"),
                         workingDirectory: "", splittable: false, profile: { kind: "local" } }
        tabModel.append({ launchJson: JSON.stringify(launch), title: launch.title })
        tabs.currentIndex = tabModel.count - 1
        if (narrow)
            sidebarVisible = false
        updateInstalling = true
        Qt.callLater(focusActivePane)
    }

    function openFiles(profile) {
        const launch = { files: true, title: profile.name, profile: profile, splittable: true }
        tabModel.append({ launchJson: JSON.stringify(launch), title: qsTr("Files on %1").arg(profile.name) })
        tabs.currentIndex = tabModel.count - 1
        if (narrow)
            sidebarVisible = false
        Qt.callLater(focusActivePane)
    }

    function showFiles() {
        if (activeIsSsh)
            currentPage.split(Qt.Horizontal, true)
        else
            showMessage(qsTr("Files"), qsTr("Focus a pane connected to an ssh host, or right-click a host in the sidebar and choose Browse files."))
    }
    readonly property bool paneShortcuts: !dialogOpen && currentPage !== null

    function sessionCount() {
        let n = 0
        for (let i = 0; i < pages.count; ++i)
            if (pages.itemAt(i))
                n += pages.itemAt(i).paneCount
        return n
    }

    function openTab(profile) {
        const launch = backend.launch(profile)
        if (launch.error) {
            showMessage(qsTr("Can't connect"), launch.error)
            return
        }
        tabModel.append({ launchJson: JSON.stringify(launch), title: launch.title })
        tabs.currentIndex = tabModel.count - 1
        if (narrow)
            sidebarVisible = false
        Qt.callLater(focusActivePane)
    }

    function closeTab(index) {
        if (index < 0 || index >= tabModel.count)
            return
        tabModel.remove(index)
        // On a phone the last session closing goes back to the list of hosts.
        if (narrow && tabModel.count === 0)
            sidebarVisible = true
        Qt.callLater(focusActivePane)
    }

    function removePage(page) {
        for (let i = 0; i < pages.count; ++i)
            if (pages.itemAt(i) === page) {
                closeTab(i)
                return
            }
    }

    function focusActivePane() {
        // Dialogs opened from Settings return to it, not to a terminal.
        if (settingsDialog.visible)
            return
        if (currentPage && currentPage.activePane)
            currentPage.activePane.focusTerminal()
        else if (sidebarVisible && !touch)
            sidebar.focusSearch()
    }

    function showMessage(heading, text, confirmLabel, onConfirm, destructive) {
        message.title = heading
        message.text = text
        message.confirmLabel = confirmLabel || ""
        message.onConfirm = onConfirm || null
        message.destructive = !!destructive
        message.open()
    }

    function requestQuit() {
        const n = sessionCount()
        if (n === 0 || !backend.settings.confirmQuit) {
            quitting = true
            Qt.quit()
            return
        }
        showMessage(qsTr("Quit Tether?"), qsTr("Close %n sessions and quit?", "", n), qsTr("Quit"), () => {
            quitting = true
            Qt.quit()
        }, true)
    }

    onClosing: close => {
        if (!quitting && sessionCount() > 0 && backend.settings.confirmQuit) {
            close.accepted = false
            requestQuit()
        }
    }

    Component.onCompleted: {
        updater.startAutomaticChecks(backend.settings.checkUpdates)
        if (backend.loadError)
            showMessage(qsTr("Saved hosts"), backend.loadError)
        // On touch screens focusing the search would pop the keyboard up.
        if (!touch)
            Qt.callLater(sidebar.focusSearch)
        // tether --open settings|new-host: straight to that screen.
        const args = Qt.application.arguments
        const show = args.indexOf("--open")
        if (show >= 0 && show + 1 < args.length) {
            if (args[show + 1] === "settings")
                Qt.callLater(() => settingsDialog.openAt(0))
            else if (args[show + 1] === "new-host")
                Qt.callLater(() => hostDialog.openFor({ kind: "ssh" }))
        }
        // tether --connect user@host[:port] [--identity key]: straight to a session.
        const at = args.indexOf("--connect")
        if (at >= 0 && at + 1 < args.length && args.indexOf("--self-test") < 0) {
            const profile = backend.quickConnect(args[at + 1])
            const key = args.indexOf("--identity")
            if (!profile.error && key >= 0 && key + 1 < args.length)
                profile.identityFile = args[key + 1]
            if (profile.error)
                showMessage(qsTr("Can't connect"), profile.error)
            else
                Qt.callLater(() => openTab(profile))
        }
    }

    ListModel { id: tabModel }

    Shortcut { sequence: "F1"; context: Qt.ApplicationShortcut; onActivated: shortcutsDialog.open() }
    Shortcut { sequence: "Ctrl+Shift+F"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts; onActivated: win.showFiles() }
    Shortcut {
        sequence: "Ctrl+Shift+A"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts
        onActivated: askDialog.openFor(win.currentPage)
    }
    Shortcut {
        sequence: "Ctrl+,"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen
        onActivated: settingsDialog.openAt(0)
    }
    Shortcut { sequence: "Ctrl+Shift+Q"; context: Qt.ApplicationShortcut; onActivated: win.requestQuit() }
    Shortcut {
        sequence: "Ctrl+Shift+K"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen
        onActivated: { win.sidebarVisible = true; sidebar.focusSearch() }
    }
    Shortcut {
        sequence: "Ctrl+Shift+B"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen
        onActivated: { win.sidebarVisible = !win.sidebarVisible; win.focusActivePane() }
    }
    Shortcut {
        sequence: "Ctrl+Shift+T"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen
        onActivated: win.openTab({ kind: "local" })
    }
    Shortcut { sequence: "Ctrl+Shift+N"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: hostDialog.openFor({ kind: "ssh" }) }
    Shortcut {
        sequence: "Ctrl+Shift+S"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts
        onActivated: snippetsDialog.openFor(win.currentPage)
    }
    Shortcut { sequence: "Ctrl+Shift+E"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts; onActivated: win.currentPage.split(Qt.Horizontal) }
    Shortcut { sequence: "Ctrl+Shift+O"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts; onActivated: win.currentPage.split(Qt.Vertical) }
    Shortcut { sequence: "Ctrl+Shift+Z"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts; onActivated: win.currentPage.toggleZoom() }
    Shortcut {
        sequence: "Ctrl+Shift+W"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts
        onActivated: win.currentPage.closePane(win.currentPage.activePane)
    }
    Shortcut { sequence: "Alt+Left"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts; onActivated: win.currentPage.moveFocus(-1, 0) }
    Shortcut { sequence: "Alt+Right"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts; onActivated: win.currentPage.moveFocus(1, 0) }
    Shortcut { sequence: "Alt+Up"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts; onActivated: win.currentPage.moveFocus(0, -1) }
    Shortcut { sequence: "Alt+Down"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts; onActivated: win.currentPage.moveFocus(0, 1) }
    Shortcut { sequence: "Alt+Shift+Left"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts; onActivated: win.currentPage.resize(-1, 0) }
    Shortcut { sequence: "Alt+Shift+Right"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts; onActivated: win.currentPage.resize(1, 0) }
    Shortcut { sequence: "Alt+Shift+Up"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts; onActivated: win.currentPage.resize(0, -1) }
    Shortcut { sequence: "Alt+Shift+Down"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts; onActivated: win.currentPage.resize(0, 1) }
    Shortcut {
        sequence: "Ctrl+PgDown"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && tabModel.count > 1
        onActivated: { tabs.currentIndex = (tabs.currentIndex + 1) % tabModel.count; win.focusActivePane() }
    }
    Shortcut {
        sequence: "Ctrl+PgUp"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && tabModel.count > 1
        onActivated: { tabs.currentIndex = (tabs.currentIndex + tabModel.count - 1) % tabModel.count; win.focusActivePane() }
    }
    Shortcut {
        sequence: "Ctrl+Shift+C"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts
        onActivated: if (win.currentPage.activePane.terminal) win.currentPage.activePane.terminal.copyClipboard()
    }
    Shortcut {
        sequence: "Ctrl+Shift+V"; context: Qt.ApplicationShortcut; enabled: win.paneShortcuts
        onActivated: if (win.currentPage.activePane.terminal) win.currentPage.activePane.terminal.pasteClipboard()
    }
    Shortcut { sequences: ["Ctrl+=", "Ctrl++"]; context: Qt.ApplicationShortcut; onActivated: backend.fontSize = backend.fontSize + 1 }
    Shortcut { sequence: "Ctrl+-"; context: Qt.ApplicationShortcut; onActivated: backend.fontSize = backend.fontSize - 1 }
    Shortcut { sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; onActivated: backend.fontSize = 13 }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    RowLayout {
        anchors.fill: parent
        spacing: 0
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        Sidebar {
            id: sidebar
            visible: win.sidebarVisible
            Layout.preferredWidth: win.narrow ? win.width : 272
            Layout.fillHeight: true
            onConnectRequested: profile => win.openTab(profile)
            onEditRequested: profile => {
                // Plugged-in devices and ~/.ssh/config hosts are saved as new hosts.
                if (profile.source === "config")
                    profile = { kind: "ssh", name: profile.name, host: profile.host }
                hostDialog.openFor(profile)
            }
            onNewRequested: kind => hostDialog.openFor({ kind: kind })
            onDeleteRequested: profile => win.showMessage(qsTr("Delete %1?").arg(profile.name),
                qsTr("This removes the saved settings. Open sessions keep running."), qsTr("Delete"), () => {
                    const error = backend.removeProfile(profile.id)
                    if (error)
                        Qt.callLater(() => win.showMessage(qsTr("Couldn't delete"), error))
                }, true)
            onSnippetsRequested: if (win.currentPage) snippetsDialog.openFor(win.currentPage)
                                 else win.showMessage(qsTr("Snippets"), qsTr("Open a session first; snippets are typed into it."))
            onShortcutsRequested: shortcutsDialog.open()
            onSettingsRequested: settingsDialog.openAt(0)
            onFilesRequested: profile => win.openFiles(profile)
            updateInstalling: win.updateInstalling
            onUpdateRequested: win.updateInstalling ? updater.restart() : updateSheet.open()
            onLeaveRequested: win.focusActivePane()
        }

        ColumnLayout {
            visible: !(win.narrow && win.sidebarVisible)
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // The toolbar: the sidebar button, the tabs, and a new tab button.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 40
                color: Style.toolbar

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: Style.separator
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    anchors.bottomMargin: 1
                    spacing: 4

                    TButton {
                        kind: "toolbar"
                        iconName: "sidebar"
                        iconSize: 17
                        checked: false
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: qsTr("Show or hide the sidebar")
                        onClicked: { win.sidebarVisible = !win.sidebarVisible; win.focusActivePane() }
                    }

                    Item {
                        clip: true
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        TabBar {
                            id: tabs
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width
                            height: 30
                            spacing: 4
                            background: null
                            contentHeight: 30

                            Repeater {
                                model: tabModel
                                TabButton {
                                    id: tab
                                    required property int index
                                    required property string title
                                    width: Math.max(Style.compact ? 92 : 120, Math.min(240, (tabs.width - (tabModel.count - 1) * tabs.spacing) / tabModel.count))
                                    height: 30
                                    hoverEnabled: true
                                    onClicked: win.focusActivePane()

                                    TapHandler {
                                        acceptedButtons: Qt.MiddleButton
                                        onTapped: win.closeTab(tab.index)
                                    }

                                    background: Rectangle {
                                        radius: 7
                                        color: tab.checked ? Qt.rgba(1, 1, 1, 0.13)
                                             : tab.hovered ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(1, 1, 1, 0)
                                        border.width: tab.checked ? 1 : 0
                                        border.color: Qt.rgba(1, 1, 1, 0.07)
                                    }

                                    contentItem: Item {
                                        Text {
                                            anchors.centerIn: parent
                                            width: Math.min(implicitWidth, parent.width - 48)
                                            text: tab.title
                                            elide: Text.ElideRight
                                            font.family: Style.fontFamily
                                            font.pixelSize: 12
                                            font.weight: tab.checked ? Font.DemiBold : Font.Normal
                                            color: tab.checked ? Style.label : Style.secondaryLabel
                                        }
                                        Rectangle {
                                            id: closeButton
                                            anchors.left: parent.left
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 18
                                            height: 18
                                            radius: 4
                                            // No hover on touch screens: the current tab always shows it.
                                            visible: tab.hovered || (Style.touch && tab.checked)
                                            color: closeArea.containsMouse ? Qt.rgba(1, 1, 1, 0.12) : "transparent"
                                            Icon {
                                                anchors.centerIn: parent
                                                name: "close"
                                                size: 10
                                                opacity: 0.7
                                            }
                                            MouseArea {
                                                id: closeArea
                                                anchors.fill: parent
                                                // A finger needs more to aim at than a pointer.
                                                anchors.margins: Style.touch ? -9 : 0
                                                hoverEnabled: true
                                                onClicked: win.closeTab(tab.index)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    TButton {
                        kind: "toolbar"
                        iconName: "folder"
                        iconSize: 16
                        visible: win.activeIsSsh && backend.sftpAvailable
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: qsTr("Files on this host (Ctrl+Shift+F)")
                        onClicked: win.showFiles()
                    }
                    TButton {
                        kind: "toolbar"
                        iconName: "sparkle"
                        iconSize: 16
                        visible: win.currentPage !== null
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: qsTr("Ask for a command (Ctrl+Shift+A)")
                        onClicked: askDialog.openFor(win.currentPage)
                    }
                    TButton {
                        kind: "toolbar"
                        iconName: "plus"
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: qsTr("New tab with a local shell")
                        onClicked: win.openTab({ kind: "local" })
                    }
                }
            }

            StackLayout {
                id: stack
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: tabs.currentIndex
                visible: tabModel.count > 0

                Repeater {
                    id: pages
                    model: tabModel
                    TabPage {
                        id: page
                        required property int index
                        onTitleChanged: tabModel.setProperty(index, "title", title)
                        onEmptied: Qt.callLater(() => win.removePage(page))
                        onFailed: message => win.showMessage(qsTr("Can't connect"), message)
                    }
                }
            }

            // With no tabs open.
            Rectangle {
                visible: tabModel.count === 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Style.window

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(440, parent.width - 48)
                    spacing: 0

                    Rectangle {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.bottomMargin: 20
                        width: 76
                        height: 76
                        radius: 18
                        gradient: Gradient {
                            GradientStop { position: 0; color: "#3A3A3E" }
                            GradientStop { position: 1; color: "#1C1C1F" }
                        }
                        border.color: Qt.rgba(1, 1, 1, 0.12)
                        Icon {
                            anchors.centerIn: parent
                            name: "terminal"
                            size: 38
                            opacity: 0.9
                        }
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: "Tether"
                        font.family: Style.fontFamily
                        font.pixelSize: 26
                        font.weight: Font.Bold
                        color: Style.label
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        lineHeight: 1.15
                        font.family: Style.fontFamily
                        font.pixelSize: Style.fontSize
                        color: Style.secondaryLabel
                        text: Style.touch ? qsTr("Pick a host, or type user@host in the search to connect.")
                                          : qsTr("Pick a host or serial port on the left, or type user@host to connect. Press F1 for shortcuts.")
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 24
                        spacing: 10
                        TButton { text: qsTr("Local shell"); iconName: "terminal"; onClicked: win.openTab({ kind: "local" }) }
                        TButton { text: qsTr("New host"); kind: "primary"; iconName: "plus"; onClicked: hostDialog.openFor({ kind: "ssh" }) }
                    }
                }
            }
        }
    }

    HostDialog {
        id: hostDialog
        onClosed: win.focusActivePane()
    }

    SnippetsDialog {
        id: snippetsDialog
        onClosed: win.focusActivePane()
    }

    AskDialog { id: askDialog }

    UpdateSheet {
        id: updateSheet
        onInstallRequested: command => win.installUpdate(command)
    }

    SettingsDialog {
        id: settingsDialog
        onClosed: win.focusActivePane()
        onShortcutsRequested: shortcutsDialog.open()
        onUpdateRequested: updateSheet.open()
        onNewProfileRequested: kind => hostDialog.openFor({ kind: kind })
        onEditProfileRequested: profile => hostDialog.openFor(profile)
        onDeleteProfileRequested: profile => sidebar.deleteRequested(profile)
        onEditSnippetRequested: snippet => snippetsDialog.openEditor(snippet)
        onDeleteSnippetRequested: snippet => win.showMessage(qsTr("Delete %1?").arg(snippet.name),
            qsTr("This snippet will be gone for good."), qsTr("Delete"), () => {
                const error = backend.removeSnippet(snippet.id)
                if (error)
                    Qt.callLater(() => win.showMessage(qsTr("Couldn't delete"), error))
            }, true)
    }

    ShortcutsDialog {
        id: shortcutsDialog
        onClosed: win.focusActivePane()
    }

    // An alert: a small sheet with the question and its buttons.
    TSheet {
        id: message
        property string text
        property string confirmLabel
        property bool destructive: false
        property var onConfirm: null
        width: Math.min(380, win.width - 40)

        contentItem: Text {
            text: message.text
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            lineHeight: 1.15
            font.family: Style.fontFamily
            font.pixelSize: Style.fontSize
            color: Style.secondaryLabel
        }
        footer: RowLayout {
            spacing: 10
            Item { implicitWidth: 10 }
            TButton {
                Layout.fillWidth: true
                Layout.bottomMargin: 20
                Layout.topMargin: 16
                visible: message.confirmLabel !== ""
                text: qsTr("Cancel")
                onClicked: message.close()
            }
            TButton {
                Layout.fillWidth: true
                Layout.bottomMargin: 20
                Layout.topMargin: 16
                kind: message.destructive ? "danger" : "primary"
                text: message.confirmLabel || qsTr("OK")
                onClicked: {
                    const action = message.onConfirm
                    message.close()
                    if (action)
                        action()
                }
            }
            Item { implicitWidth: 10 }
        }
        onClosed: win.focusActivePane()
    }
}
