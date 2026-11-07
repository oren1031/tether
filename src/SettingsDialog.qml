import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Tether

// Preferences, laid out like macOS System Settings: sections on the left and
// grouped rows on the right. Every change saves at once.
TSheet {
    id: dialog

    signal newProfileRequested(string kind)
    signal editProfileRequested(var profile)
    signal deleteProfileRequested(var profile)
    signal editSnippetRequested(var snippet)
    signal deleteSnippetRequested(var snippet)
    signal shortcutsRequested()
    signal updateRequested()

    property int section: 0
    property string error: ""

    width: Math.min(820, Overlay.overlay ? Overlay.overlay.width - 40 : 820)
    height: Math.min(600, Overlay.overlay ? Overlay.overlay.height - 40 : 600)
    title: qsTr("Settings")
    padding: 0
    topPadding: 0
    header: null

    readonly property var sections: [
        { name: qsTr("General"), icon: "gear", color: Style.gray },
        { name: qsTr("Appearance"), icon: "palette", color: Style.blue },
        { name: qsTr("Terminal"), icon: "terminal", color: "#3A3A3C" },
        { name: qsTr("Hosts"), icon: "server", color: Style.indigo },
        { name: qsTr("Snippets"), icon: "snippets", color: Style.orange },
        { name: qsTr("Vault"), icon: "key", color: "#D4A017" },
        { name: qsTr("Sessions"), icon: "infinity", color: Style.green },
        { name: qsTr("Sync"), icon: "cloud", color: "#32ADE6" },
        { name: qsTr("AI"), icon: "sparkle", color: Style.purple }
    ]

    function openAt(index) {
        section = index || 0
        error = ""
        open()
    }

    function set(key, value) {
        error = backend.setSetting(key, value)
    }

    // ── Building blocks ─────────────────────────────────────────────────────

    // A rounded group of rows, like an inset list in System Settings.
    component Group: ColumnLayout {
        id: group
        property string title: ""
        property string footnote: ""
        default property alias rows: body.data
        Layout.fillWidth: true
        spacing: 6

        Text {
            visible: group.title !== ""
            Layout.leftMargin: 4
            text: group.title
            font.family: Style.fontFamily
            font.pixelSize: Style.fontSize
            font.weight: Font.DemiBold
            color: Style.label
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: body.implicitHeight
            radius: 10
            color: Qt.rgba(1, 1, 1, 0.04)
            border.color: Qt.rgba(1, 1, 1, 0.07)
            ColumnLayout {
                id: body
                width: parent.width
                spacing: 0
            }
        }
        Text {
            visible: group.footnote !== ""
            Layout.fillWidth: true
            Layout.leftMargin: 4
            text: group.footnote
            wrapMode: Text.Wrap
            font.family: Style.fontFamily
            font.pixelSize: Style.smallFontSize
            color: Style.tertiaryLabel
        }
    }

    // A row in a Group: a label, an optional note under it, and a control on
    // the right. The last row in a group sets last so it draws no line.
    component Row_: Item {
        id: settingRow
        property string label: ""
        property string note: ""
        property bool last: false
        default property alias control: slot.data
        Layout.fillWidth: true
        implicitHeight: Math.max(44, texts.implicitHeight + 20, slot.childrenRect.height + 16)

        ColumnLayout {
            id: texts
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.right: slot.left
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text {
                Layout.fillWidth: true
                text: settingRow.label
                elide: Text.ElideRight
                font.family: Style.fontFamily
                font.pixelSize: Style.fontSize
                color: Style.label
            }
            Text {
                visible: settingRow.note !== ""
                Layout.fillWidth: true
                text: settingRow.note
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                font.family: Style.fontFamily
                font.pixelSize: Style.smallFontSize
                color: Style.secondaryLabel
            }
        }
        Item {
            id: slot
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            width: childrenRect.width
            height: childrenRect.height
        }
        Rectangle {
            visible: !settingRow.last
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.right: parent.right
            height: 1
            color: Qt.rgba(1, 1, 1, 0.06)
        }
    }

    // A section's scrolling page with its big title.
    component Page: ScrollView {
        id: page
        property string heading: ""
        default property alias content: column.data
        clip: true
        contentWidth: availableWidth
        ScrollBar.vertical: TScrollBar { parent: page; x: page.width - width; height: page.height }

        ColumnLayout {
            id: column
            x: 28
            width: page.availableWidth - 56
            spacing: 22
            Text {
                Layout.topMargin: 24
                text: page.heading
                font.family: Style.fontFamily
                font.pixelSize: Style.fontSize + 9
                font.weight: Font.Bold
                color: Style.label
            }
        }
    }

    component Badge: Rectangle {
        property string icon
        property color tone
        implicitWidth: 24
        implicitHeight: 24
        radius: 6
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.lighter(tone, 1.15) }
            GradientStop { position: 1; color: tone }
        }
        Icon {
            anchors.centerIn: parent
            name: parent.icon
            size: 14
        }
    }

    // ── Layout ──────────────────────────────────────────────────────────────

    contentItem: RowLayout {
        spacing: 0

        // Sections.
        Rectangle {
            Layout.preferredWidth: 200
            Layout.fillHeight: true
            color: Qt.rgba(0, 0, 0, 0.18)
            topLeftRadius: Style.radiusLarge
            bottomLeftRadius: Style.radiusLarge

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                anchors.topMargin: 18
                spacing: 2

                Text {
                    Layout.leftMargin: 8
                    Layout.bottomMargin: 10
                    text: qsTr("Settings")
                    font.family: Style.fontFamily
                    font.pixelSize: Style.titleFontSize
                    font.weight: Font.Bold
                    color: Style.label
                }

                Repeater {
                    model: dialog.sections
                    delegate: ItemDelegate {
                        id: nav
                        required property int index
                        required property var modelData
                        Layout.fillWidth: true
                        topPadding: 5
                        bottomPadding: 5
                        leftPadding: 6
                        rightPadding: 6
                        hoverEnabled: true
                        onClicked: dialog.section = index
                        background: Rectangle {
                            radius: 7
                            color: dialog.section === nav.index ? Style.accent
                                 : nav.hovered ? Qt.rgba(1, 1, 1, 0.05) : "transparent"
                        }
                        contentItem: RowLayout {
                            spacing: 10
                            Badge { icon: nav.modelData.icon; tone: nav.modelData.color }
                            Text {
                                Layout.fillWidth: true
                                text: nav.modelData.name
                                font.family: Style.fontFamily
                                font.pixelSize: Style.fontSize
                                font.weight: Font.Medium
                                color: dialog.section === nav.index ? Style.onAccent : Style.label
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                Text {
                    visible: dialog.error !== ""
                    Layout.fillWidth: true
                    Layout.leftMargin: 6
                    wrapMode: Text.Wrap
                    font.family: Style.fontFamily
                    font.pixelSize: Style.smallFontSize
                    color: Style.red
                    text: dialog.error
                }
                TButton {
                    Layout.fillWidth: true
                    text: qsTr("Done")
                    kind: "primary"
                    onClicked: dialog.close()
                }
            }
        }

        Rectangle {
            Layout.fillHeight: true
            implicitWidth: 1
            color: Style.separator
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: dialog.section

            // ── General ──
            Page {
                heading: qsTr("General")

                Group {
                    Row_ {
                        label: qsTr("Local shell")
                        note: qsTr("Blank uses your login shell.")
                        TField {
                            id: shellField
                            width: 220
                            text: backend.settings.shell
                            placeholderText: Qt.platform.os === "linux" ? "/usr/bin/bash" : ""
                            onEditingFinished: if (text !== backend.settings.shell) dialog.set("shell", text)
                        }
                    }
                    Row_ {
                        label: qsTr("Ask before quitting with sessions open")
                        last: true
                        TSwitch {
                            checked: backend.settings.confirmQuit
                            onToggled: dialog.set("confirmQuit", checked)
                        }
                    }
                }

                Group {
                    Row_ {
                        label: qsTr("Session logs")
                        note: backend.logDirectory
                        last: true
                        Item { width: 1; height: 1 }
                    }
                }

                Group {
                    title: qsTr("Updates")
                    Row_ {
                        label: qsTr("Tether %1").arg(updater.currentVersion)
                        note: !updater.configured ? qsTr("This build has no update source.")
                            : updater.message ? updater.message
                            : qsTr("Updates come from GitHub releases.")
                        RowLayout {
                            spacing: 6
                            TButton {
                                visible: updater.available
                                kind: "primary"
                                text: qsTr("Update…")
                                onClicked: { dialog.close(); dialog.updateRequested() }
                            }
                            TButton {
                                enabled: updater.configured && updater.state !== "checking"
                                text: qsTr("Check now")
                                onClicked: updater.check(false)
                            }
                        }
                    }
                    Row_ {
                        label: qsTr("Check for updates automatically")
                        last: true
                        TSwitch {
                            enabled: updater.configured
                            checked: backend.settings.checkUpdates
                            onToggled: {
                                dialog.set("checkUpdates", checked)
                                updater.startAutomaticChecks(checked)
                            }
                        }
                    }
                }

                Group {
                    Row_ {
                        label: qsTr("Keyboard shortcuts")
                        TButton {
                            text: qsTr("Show…")
                            onClicked: dialog.shortcutsRequested()
                        }
                    }
                    Row_ {
                        label: qsTr("Restore default settings")
                        note: qsTr("Saved hosts and snippets are kept.")
                        last: true
                        TButton {
                            text: qsTr("Restore")
                            onClicked: {
                                dialog.error = backend.resetSettings()
                                backend.fontSize = 13
                            }
                        }
                    }
                }
            }

            // ── Appearance ──
            Page {
                heading: qsTr("Appearance")

                Group {
                    title: qsTr("Accent color")
                    footnote: qsTr("Used for selections, buttons, and the focused pane.")
                    Item {
                        Layout.fillWidth: true
                        implicitHeight: swatches.implicitHeight + 28
                        Flow {
                            id: swatches
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.margins: 14
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 4
                            Repeater {
                                model: Style.accents
                                delegate: Column {
                                    id: swatch
                                    required property var modelData
                                    readonly property bool chosen: backend.settings.accent === modelData.key
                                    readonly property color tone: modelData.key === "omarchy" ? theme.accent : modelData.color
                                    spacing: 6
                                    width: 52
                                    Rectangle {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        width: 26
                                        height: 26
                                        radius: 13
                                        color: swatch.tone
                                        border.width: swatch.modelData.key === "omarchy" ? 2 : 0
                                        border.color: Qt.rgba(1, 1, 1, 0.5)
                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 34
                                            height: 34
                                            radius: 17
                                            color: "transparent"
                                            border.width: 2
                                            border.color: swatch.tone
                                            visible: swatch.chosen
                                        }
                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 8
                                            height: 8
                                            radius: 4
                                            color: "white"
                                            visible: swatch.chosen
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            anchors.margins: -4
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: dialog.set("accent", swatch.modelData.key)
                                        }
                                    }
                                    Text {
                                        width: parent.width
                                        horizontalAlignment: Text.AlignHCenter
                                        text: swatch.modelData.name
                                        wrapMode: Text.Wrap
                                        font.family: Style.fontFamily
                                        font.pixelSize: Style.smallFontSize
                                        color: swatch.chosen ? Style.label : Style.tertiaryLabel
                                    }
                                }
                            }
                        }
                    }
                }

                Group {
                    title: qsTr("Text")
                    Row_ {
                        label: qsTr("Font")
                        TCombo {
                            width: 230
                            readonly property var families: [qsTr("Default")].concat(theme.fontFamilies(false))
                            model: families
                            currentIndex: backend.settings.interfaceFont ? Math.max(0, families.indexOf(backend.settings.interfaceFont)) : 0
                            onActivated: index => dialog.set("interfaceFont", index === 0 ? "" : families[index])
                        }
                    }
                    Row_ {
                        label: qsTr("Size")
                        last: true
                        TSegmented {
                            model: [{ value: 12, text: qsTr("Small") }, { value: 13, text: qsTr("Medium") },
                                    { value: 14, text: qsTr("Large") }, { value: 16, text: qsTr("Larger") }]
                            value: backend.settings.interfaceFontSize
                            onActivated: v => dialog.set("interfaceFontSize", v)
                        }
                    }
                }
            }

            // ── Terminal ──
            Page {
                heading: qsTr("Terminal")

                // A sample of the chosen colors and font.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: preview.implicitHeight + 28
                    radius: 10
                    readonly property var colors: { theme.terminalScheme; return theme.schemeColors(backend.settings.terminalScheme) }
                    color: Style.terminalBackground
                    border.color: Qt.rgba(1, 1, 1, 0.1)
                    Column {
                        id: preview
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 14
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: backend.settings.lineSpacing
                        readonly property var c: parent.colors
                        readonly property color fg: c.foreground || "#e6e6e6"
                        readonly property var pal: c.palette || []
                        Text {
                            textFormat: Text.RichText
                            font.family: Style.terminalFamily
                            font.pixelSize: backend.fontSize
                            color: preview.fg
                            text: `<span style="color:${preview.pal[3]}">~/projects</span> <span style="color:${preview.pal[1]}">❯</span> ssh deploy@web-01`
                        }
                        Text {
                            textFormat: Text.RichText
                            font.family: Style.terminalFamily
                            font.pixelSize: backend.fontSize
                            font.bold: backend.settings.boldText
                            color: preview.fg
                            text: `<span style="color:${preview.pal[1]}">●</span> nginx.service — <span style="color:${preview.pal[1]}">active (running)</span>`
                        }
                        Text {
                            textFormat: Text.RichText
                            font.family: Style.terminalFamily
                            font.pixelSize: backend.fontSize
                            color: preview.fg
                            text: `<span style="color:${preview.pal[0]}">error</span> <span style="color:${preview.pal[2]}">warn</span> <span style="color:${preview.pal[4]}">debug</span> <span style="color:${preview.pal[5]}">info</span>`
                        }
                    }
                }

                Group {
                    title: qsTr("Colors")
                    Item {
                        Layout.fillWidth: true
                        implicitHeight: schemes.implicitHeight + 28
                        Flow {
                            id: schemes
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.margins: 14
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 12
                            Repeater {
                                model: ["omarchy"].concat(theme.builtinSchemes())
                                delegate: Column {
                                    id: card
                                    required property string modelData
                                    readonly property bool chosen: backend.settings.terminalScheme === modelData
                                    readonly property var colors: { theme.terminalScheme; return theme.schemeColors(modelData) }
                                    spacing: 5
                                    width: 112
                                    Rectangle {
                                        width: parent.width
                                        height: 62
                                        radius: 8
                                        color: card.colors.background || "black"
                                        border.width: card.chosen ? 2 : 1
                                        border.color: card.chosen ? Style.accent : Qt.rgba(1, 1, 1, 0.12)
                                        Text {
                                            x: 9
                                            y: 8
                                            text: "~ ❯ ls"
                                            font.family: Style.terminalFamily
                                            font.pixelSize: 11
                                            color: card.colors.foreground || "white"
                                        }
                                        Row {
                                            x: 9
                                            anchors.bottom: parent.bottom
                                            anchors.bottomMargin: 9
                                            spacing: 4
                                            Repeater {
                                                model: card.colors.palette || []
                                                delegate: Rectangle {
                                                    required property var modelData
                                                    width: 10
                                                    height: 10
                                                    radius: 5
                                                    color: modelData
                                                }
                                            }
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: dialog.set("terminalScheme", card.modelData)
                                        }
                                    }
                                    Text {
                                        width: parent.width
                                        horizontalAlignment: Text.AlignHCenter
                                        elide: Text.ElideRight
                                        text: card.modelData === "omarchy" ? qsTr("Omarchy theme") : card.modelData
                                        font.family: Style.fontFamily
                                        font.pixelSize: Style.smallFontSize
                                        font.weight: card.chosen ? Font.DemiBold : Font.Normal
                                        color: card.chosen ? Style.label : Style.secondaryLabel
                                    }
                                }
                            }
                        }
                    }
                }

                Group {
                    title: qsTr("Text")
                    Row_ {
                        label: qsTr("Font")
                        TCombo {
                            width: 230
                            readonly property var families: [qsTr("System monospace")].concat(theme.fontFamilies(true))
                            model: families
                            currentIndex: backend.settings.terminalFont ? Math.max(0, families.indexOf(backend.settings.terminalFont)) : 0
                            onActivated: index => dialog.set("terminalFont", index === 0 ? "" : families[index])
                        }
                    }
                    Row_ {
                        label: qsTr("Size")
                        note: qsTr("Ctrl+= and Ctrl+- change it too.")
                        TStepper {
                            value: backend.fontSize
                            from: 7
                            to: 40
                            suffix: " pt"
                            onModified: v => backend.fontSize = v
                        }
                    }
                    Row_ {
                        label: qsTr("Line spacing")
                        TStepper {
                            value: backend.settings.lineSpacing
                            from: 0
                            to: 12
                            suffix: " px"
                            onModified: v => dialog.set("lineSpacing", v)
                        }
                    }
                    Row_ {
                        label: qsTr("Bold text")
                        last: true
                        TSwitch {
                            checked: backend.settings.boldText
                            onToggled: dialog.set("boldText", checked)
                        }
                    }
                }

                Group {
                    title: qsTr("Behavior")
                    Row_ {
                        label: qsTr("Blinking cursor")
                        TSwitch {
                            checked: backend.settings.blinkingCursor
                            onToggled: dialog.set("blinkingCursor", checked)
                        }
                    }
                    Row_ {
                        label: qsTr("Dim panes that aren't focused")
                        last: true
                        TSwitch {
                            checked: backend.settings.paneDimming
                            onToggled: dialog.set("paneDimming", checked)
                        }
                    }
                }
                Item { implicitHeight: 4 }
            }

            // ── Hosts ──
            Page {
                heading: qsTr("Hosts")

                Group {
                    footnote: qsTr("Hosts from ~/.ssh/config show in the sidebar too; save one to change it here.")
                    Repeater {
                        model: backend.profiles
                        delegate: Row_ {
                            id: hostRow
                            required property int index
                            required property var modelData
                            label: modelData.name
                            note: modelData.kind === "ssh"
                                  ? (modelData.user ? modelData.user + "@" : "") + modelData.host
                                    + (modelData.port && modelData.port !== 22 ? ":" + modelData.port : "")
                                  : modelData.device + " · " + modelData.baud
                            last: index === backend.profiles.length - 1
                            RowLayout {
                                spacing: 6
                                TButton { text: qsTr("Edit…"); onClicked: dialog.editProfileRequested(hostRow.modelData) }
                                TButton {
                                    kind: "toolbar"
                                    iconName: "close"
                                    iconSize: 12
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("Delete")
                                    onClicked: dialog.deleteProfileRequested(hostRow.modelData)
                                }
                            }
                        }
                    }
                    Row_ {
                        visible: backend.profiles.length === 0
                        label: qsTr("No saved hosts or serial ports yet.")
                        last: true
                        Item { width: 1; height: 1 }
                    }
                }

                RowLayout {
                    spacing: 10
                    TButton { iconName: "plus"; text: qsTr("New host"); onClicked: dialog.newProfileRequested("ssh") }
                    TButton { iconName: "plus"; text: qsTr("New serial port"); onClicked: dialog.newProfileRequested("serial") }
                }
                Item { implicitHeight: 4 }
            }

            // ── Snippets ──
            Page {
                heading: qsTr("Snippets")

                Group {
                    footnote: qsTr("Send snippets to a session with Ctrl+Shift+S.")
                    Repeater {
                        model: backend.snippets
                        delegate: Row_ {
                            id: snippetRow
                            required property int index
                            required property var modelData
                            label: modelData.name
                            note: modelData.text.replace(/\n/g, " ⏎ ")
                            last: index === backend.snippets.length - 1
                            RowLayout {
                                spacing: 6
                                TButton { text: qsTr("Edit…"); onClicked: dialog.editSnippetRequested(snippetRow.modelData) }
                                TButton {
                                    kind: "toolbar"
                                    iconName: "close"
                                    iconSize: 12
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("Delete")
                                    onClicked: dialog.deleteSnippetRequested(snippetRow.modelData)
                                }
                            }
                        }
                    }
                    Row_ {
                        visible: backend.snippets.length === 0
                        label: qsTr("No snippets yet.")
                        last: true
                        Item { width: 1; height: 1 }
                    }
                }

                RowLayout {
                    TButton { iconName: "plus"; text: qsTr("New snippet"); onClicked: dialog.editSnippetRequested(null) }
                }
                Item { implicitHeight: 4 }
            }

            // ── Vault ──
            Page {
                heading: qsTr("Vault")

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.family: Style.fontFamily
                    font.pixelSize: Style.fontSize
                    color: Style.secondaryLabel
                    text: qsTr("Logins hold a user, a key file, and a password or key passphrase. Pick one for a host and ssh signs in with it. Passwords are kept in your system keyring, never in Tether's files.")
                }

                Group {
                    visible: !backend.keyringAvailable
                    Row_ {
                        label: qsTr("No keyring found")
                        note: qsTr("Saving passwords needs secret-tool (the libsecret package) and a keyring such as GNOME Keyring or KeePassXC. Logins without passwords still work.")
                        last: true
                        Item { width: 1; height: 1 }
                    }
                }

                Group {
                    Repeater {
                        model: backend.identities
                        delegate: Row_ {
                            id: loginRow
                            required property int index
                            required property var modelData
                            label: modelData.name
                            note: [modelData.user, modelData.identityFile,
                                   modelData.hasPassword ? qsTr("password in keyring") : ""].filter(x => x).join("  ·  ")
                            last: index === backend.identities.length - 1
                            RowLayout {
                                spacing: 6
                                TButton { text: qsTr("Edit…"); onClicked: loginSheet.edit(loginRow.modelData) }
                                TButton {
                                    kind: "toolbar"
                                    iconName: "close"
                                    iconSize: 12
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("Delete")
                                    onClicked: dialog.error = backend.removeIdentity(loginRow.modelData.id)
                                }
                            }
                        }
                    }
                    Row_ {
                        visible: backend.identities.length === 0
                        label: qsTr("No logins yet.")
                        last: true
                        Item { width: 1; height: 1 }
                    }
                }

                RowLayout {
                    TButton { iconName: "plus"; text: qsTr("New login"); onClicked: loginSheet.edit(null) }
                }
                Item { implicitHeight: 4 }
            }

            // ── Sessions ──
            Page {
                heading: qsTr("Sessions")

                Group {
                    title: qsTr("Keeping sessions alive")
                    footnote: qsTr("For ssh hosts, turn on “Keep the session running on the server” and “Reconnect when the connection drops” in each host's settings, or connect with mosh.")
                    Row_ {
                        label: qsTr("Run local shells in tmux")
                        note: backend.tmuxAvailable
                              ? qsTr("They keep running when Tether closes; reopen them from Detached sessions in the sidebar.")
                              : qsTr("Install tmux to use this: sudo pacman -S tmux")
                        TSwitch {
                            enabled: backend.tmuxAvailable
                            checked: backend.settings.localTmux
                            onToggled: dialog.set("localTmux", checked)
                        }
                    }
                    Row_ {
                        label: qsTr("Share one login per host")
                        note: qsTr("New panes, the file browser, and reconnects reuse an open connection, so you type a password once. Needed for the file browser with password logins.")
                        last: true
                        TSwitch {
                            checked: backend.settings.shareConnections
                            onToggled: dialog.set("shareConnections", checked)
                        }
                    }
                }
            }

            // ── Sync ──
            Page {
                heading: qsTr("Sync")

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.family: Style.fontFamily
                    font.pixelSize: Style.fontSize
                    color: Style.secondaryLabel
                    text: qsTr("Pick a folder that Syncthing, Dropbox, Nextcloud, or a git repository already keeps in step between your computers. Tether keeps your hosts, snippets, and vault logins there in tether-hosts.json, and picks up changes made on other computers. Passwords stay in each computer's keyring and are never synced.")
                }

                Group {
                    Row_ {
                        label: backend.settings.syncFolder ? backend.settings.syncFolder : qsTr("Not syncing")
                        note: backend.syncStatus
                        last: true
                        RowLayout {
                            spacing: 6
                            TButton {
                                text: backend.settings.syncFolder ? qsTr("Change…") : qsTr("Choose folder…")
                                kind: backend.settings.syncFolder ? "default" : "primary"
                                onClicked: folderPicker.open()
                            }
                            TButton {
                                visible: backend.settings.syncFolder !== ""
                                text: qsTr("Stop")
                                onClicked: dialog.set("syncFolder", "")
                            }
                        }
                    }
                }

                FolderDialog {
                    id: folderPicker
                    title: qsTr("Folder to sync hosts through")
                    onAccepted: dialog.set("syncFolder", decodeURIComponent(selectedFolder.toString().replace(/^file:\/\//, "")))
                }
            }

            // ── AI ──
            Page {
                heading: qsTr("AI")

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.family: Style.fontFamily
                    font.pixelSize: Style.fontSize
                    color: Style.secondaryLabel
                    text: qsTr("Press Ctrl+Shift+A in a session and describe what you want; Claude answers with a command to insert or run. Only your request and the kind of session are sent, never the terminal's contents.")
                }

                Group {
                    title: qsTr("Connect with")
                    Row_ {
                        label: qsTr("Account")
                        note: assistant.route === "claude-code" ? qsTr("Using your Claude subscription through Claude Code.")
                            : assistant.route === "api" ? qsTr("Using your Anthropic API key, billed per request.")
                            : qsTr("Not connected yet; set up one of the options below.")
                        last: true
                        TSegmented {
                            model: [{ value: "auto", text: qsTr("Automatic") },
                                    { value: "claude-code", text: qsTr("Subscription") },
                                    { value: "api", text: qsTr("API key") }]
                            value: backend.settings.aiProvider
                            onActivated: v => dialog.set("aiProvider", v)
                        }
                    }
                }

                Group {
                    title: qsTr("Claude subscription (Pro or Max)")
                    footnote: qsTr("Tether asks your own Claude Code, which signs in with your Claude account, so answers count toward your plan's usage instead of an API bill.")
                    Row_ {
                        label: assistant.claudeCode ? qsTr("Claude Code found") : qsTr("Claude Code isn't installed")
                        note: assistant.claudeCode ? assistant.claudeCode + "  ·  " + qsTr("If it isn't signed in yet, run claude in a terminal and log in.")
                                                   : qsTr("Install it from claude.com/code, run claude once, and log in with your Claude account.")
                        last: true
                        TButton {
                            visible: !assistant.claudeCode
                            text: qsTr("Get Claude Code")
                            onClicked: Qt.openUrlExternally("https://claude.com/code")
                        }
                    }
                }

                Group {
                    title: qsTr("Anthropic API key")
                    Row_ {
                        label: qsTr("API key")
                        note: assistant.keySource === "environment" ? qsTr("Using ANTHROPIC_API_KEY from the environment.")
                            : assistant.keySource === "saved" ? qsTr("Saved in a file only you can read.")
                            : qsTr("Get one at console.anthropic.com.")
                        RowLayout {
                            spacing: 6
                            TField {
                                id: keyField
                                visible: assistant.keySource !== "environment"
                                width: 200
                                Layout.preferredWidth: 200
                                echoMode: TextInput.Password
                                placeholderText: assistant.keySource === "saved" ? "••••••••" : "sk-ant-…"
                                onAccepted: saveKey.clicked()
                            }
                            TButton {
                                id: saveKey
                                visible: assistant.keySource !== "environment"
                                text: qsTr("Save")
                                enabled: keyField.text !== ""
                                onClicked: {
                                    dialog.error = assistant.saveKey(keyField.text)
                                    if (!dialog.error)
                                        keyField.text = ""
                                }
                            }
                            TButton {
                                visible: assistant.keySource === "saved"
                                text: qsTr("Remove")
                                onClicked: dialog.error = assistant.saveKey("")
                            }
                        }
                    }
                    Row_ {
                        label: qsTr("Model")
                        note: qsTr("Claude Opus 5.5, at low effort for quick answers.")
                        last: true
                        Item { width: 1; height: 1 }
                    }
                }
            }
        }
    }

    // Adds or edits a vault login.
    TSheet {
        id: loginSheet
        parent: Overlay.overlay
        property var login: null
        property string sheetError: ""
        width: Math.min(460, Overlay.overlay ? Overlay.overlay.width - 40 : 460)
        title: login ? qsTr("Edit %1").arg(login.name) : qsTr("New login")

        function edit(existing) {
            login = existing
            sheetError = ""
            loginName.text = existing ? existing.name : ""
            loginUser.text = existing ? existing.user : ""
            loginKey.text = existing ? existing.identityFile : ""
            loginPassword.text = ""
            forget.checked = false
            open()
            loginName.forceActiveFocus()
        }

        function save() {
            sheetError = backend.saveIdentity({ id: login ? login.id : "", name: loginName.text, user: loginUser.text,
                                                identityFile: loginKey.text }, loginPassword.text, forget.checked)
            if (!sheetError)
                close()
        }

        contentItem: GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 10
            component L: Text {
                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                font.family: Style.fontFamily
                font.pixelSize: Style.fontSize
                color: Style.secondaryLabel
            }
            L { text: qsTr("Name") }
            TField { id: loginName; Layout.fillWidth: true; placeholderText: qsTr("Work servers"); onAccepted: loginSheet.save() }
            L { text: qsTr("User") }
            TField { id: loginUser; Layout.fillWidth: true; placeholderText: qsTr("deploy"); onAccepted: loginSheet.save() }
            L { text: qsTr("Key file") }
            TField { id: loginKey; Layout.fillWidth: true; placeholderText: qsTr("~/.ssh/id_ed25519 (optional)"); onAccepted: loginSheet.save() }
            L { text: qsTr("Password") }
            TField {
                id: loginPassword
                Layout.fillWidth: true
                enabled: backend.keyringAvailable && !forget.checked
                echoMode: TextInput.Password
                placeholderText: loginSheet.login && loginSheet.login.hasPassword ? qsTr("Saved; type to replace")
                                                                                  : qsTr("Password or key passphrase")
                onAccepted: loginSheet.save()
            }
            Item { implicitWidth: 1; visible: forget.visible }
            TCheck {
                id: forget
                visible: loginSheet.login !== null && loginSheet.login.hasPassword
                text: qsTr("Remove the saved password")
            }
            Text {
                Layout.columnSpan: 2
                Layout.fillWidth: true
                visible: loginSheet.sheetError !== ""
                wrapMode: Text.Wrap
                font.family: Style.fontFamily
                font.pixelSize: Style.fontSize
                color: Style.red
                text: loginSheet.sheetError
            }
        }
        footer: RowLayout {
            spacing: 10
            Item { Layout.fillWidth: true }
            TButton { Layout.bottomMargin: 20; text: qsTr("Cancel"); onClicked: loginSheet.close() }
            TButton { Layout.bottomMargin: 20; Layout.rightMargin: 20; kind: "primary"; text: qsTr("Save"); onClicked: loginSheet.save() }
        }
    }
}
