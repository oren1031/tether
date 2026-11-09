import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Tether

// Everything there is to connect to: a local shell, saved hosts and serial
// ports, devices plugged in now, and hosts from ~/.ssh/config. Typing filters
// the list, and a user@host or /dev path that matches nothing becomes a quick
// connection.
Rectangle {
    id: side

    color: Style.sidebar

    signal connectRequested(var profile)
    signal editRequested(var profile)
    signal deleteRequested(var profile)
    signal newRequested(string kind)
    signal snippetsRequested()
    signal leaveRequested()
    signal shortcutsRequested()
    signal settingsRequested()
    signal filesRequested(var profile)
    signal updateRequested()
    property bool updateInstalling: false

    function focusSearch() {
        search.forceActiveFocus()
        search.selectAll()
    }

    function current() {
        return list.currentIndex >= 0 ? items.get(list.currentIndex) : null
    }

    function sshDetail(p) {
        return (p.user ? p.user + "@" : "") + p.host + (p.port && p.port !== 22 ? ":" + p.port : "")
            + (p.forwards && p.forwards.length ? "  ·  ⇄ " + p.forwards.length : "")
    }

    function serialDetail(p) {
        return p.device.replace(/^\/dev\/serial\/by-id\//, "") + " · " + p.baud
    }

    function rebuild() {
        const query = search.text.trim()
        const needle = query.toLowerCase()
        const rows = []
        let exact = false
        const add = (section, name, detail, profile, saved, warn, badge) => {
            if (needle && !(name + " " + detail).toLowerCase().includes(needle))
                return
            if (name.toLowerCase() === needle)
                exact = true
            rows.push({ section: section, name: name, detail: detail, warn: warn || "", badge: badge || "",
                        profile: JSON.stringify(profile), saved: saved })
        }

        add(qsTr("Local"), qsTr("Local shell"), qsTr("A shell on this computer"), { kind: "local" }, false, "", "local")
        for (const p of backend.profiles)
            if (p.kind === "ssh")
                add(qsTr("Hosts"), p.name, sshDetail(p), p, true, "", "ssh")
        for (const p of backend.profiles)
            if (p.kind === "serial")
                add(qsTr("Serial ports"), p.name, serialDetail(p), p, true, "", "serial")
        for (const d of serialPorts.devices)
            add(qsTr("Plugged in"), d.label, d.path + " · 115200",
                { kind: "serial", name: d.label, device: d.stablePath, baud: 115200, dataBits: 8,
                  parity: "none", stopBits: 1, flow: "none", enter: "cr", echo: false, log: false },
                false, d.accessible ? "" : qsTr("No permission"), "device")
        for (const t of backend.tmuxSessions)
            add(qsTr("Detached sessions"), t.name.replace(/^tether-/, ""), qsTr("Started %1 · %2").arg(t.created).arg(t.path),
                { kind: "tmux", session: t.name }, false, "", "tmux")
        for (const h of backend.configHosts)
            add(qsTr("From ~/.ssh/config"), h.name, h.detail, h, false, "", "config")

        if (query && !exact) {
            const quick = backend.quickConnect(query)
            if (!quick.error)
                rows.unshift({ section: qsTr("Quick connect"), name: qsTr("Connect to %1").arg(query),
                               detail: quick.kind === "serial" ? serialDetail(quick) : sshDetail(quick),
                               warn: "", badge: "quick", profile: JSON.stringify(quick), saved: false })
        }

        items.clear()
        for (const row of rows)
            items.append(row)
        list.currentIndex = rows.length ? 0 : -1
    }

    function connectCurrent() {
        const row = current()
        if (row)
            connectRequested(JSON.parse(row.profile))
    }

    function editCurrent() {
        const row = current()
        if (row && ["local", "tmux"].indexOf(JSON.parse(row.profile).kind) < 0)
            editRequested(JSON.parse(row.profile))
    }

    function deleteCurrent() {
        const row = current()
        if (row && row.saved)
            deleteRequested(JSON.parse(row.profile))
    }

    Component.onCompleted: rebuild()
    Connections {
        target: backend
        function onProfilesChanged() { side.rebuild() }
        function onConfigHostsChanged() { side.rebuild() }
        function onTmuxSessionsChanged() { side.rebuild() }
    }
    Connections {
        target: serialPorts
        function onDevicesChanged() { side.rebuild() }
    }
    Connections {
        target: translator
        function onChanged() { side.rebuild() }
    }

    ListModel { id: items }

    // Local tmux sessions come and go outside Tether, so look now and then.
    Timer {
        interval: 4000
        running: backend.tmuxAvailable
        repeat: true
        triggeredOnStart: true
        onTriggered: backend.refreshTmux()
    }

    function badgeColor(badge) {
        switch (badge) {
        case "local": return Style.gray
        case "ssh": return Style.blue
        case "serial": return Style.orange
        case "device": return Style.green
        case "config": return Style.indigo
        case "quick": return Style.purple
        case "tmux": return Style.green
        }
        return Style.gray
    }

    function badgeIcon(badge) {
        switch (badge) {
        case "local": return "terminal"
        case "ssh": case "config": return "server"
        case "serial": case "device": return "cable"
        case "quick": return "bolt"
        case "tmux": return "infinity"
        }
        return "terminal"
    }

    // The right edge, where the sidebar meets the tabs.
    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Style.separator
        z: 2
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.rightMargin: 1
        spacing: 0

        TField {
            id: search
            Layout.fillWidth: true
            Layout.margins: 10
            Layout.bottomMargin: 6
            iconName: "search"
            label: qsTr("Search hosts")
            placeholderText: Qt.platform.os === "windows" ? qsTr("Search, user@host, or COM3")
                           : Qt.platform.os === "android" ? qsTr("Search, or user@host")
                           : qsTr("Search, user@host, or /dev/…")
            onTextChanged: side.rebuild()
            onAccepted: side.connectCurrent()
            Keys.onDownPressed: { list.forceActiveFocus(); list.incrementCurrentIndex() }
            Keys.onUpPressed: list.decrementCurrentIndex()
            Keys.onEscapePressed: {
                text = ""
                side.leaveRequested()
            }
        }

        ListView {
            id: list
            objectName: "hostList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            leftMargin: 8
            rightMargin: 8
            bottomMargin: 8
            clip: true
            model: items
            keyNavigationWraps: false
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Hosts and serial ports")
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            ScrollBar.vertical: TScrollBar {}

            section.property: "section"
            section.delegate: Text {
                required property string section
                width: ListView.view.width - 16
                topPadding: 12
                bottomPadding: 4
                leftPadding: 8
                rightPadding: 8
                text: section
                font.family: Style.fontFamily
                font.pixelSize: Style.smallFontSize
                font.weight: Font.Bold
                color: Style.tertiaryLabel
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignLeft
                Accessible.role: Accessible.Heading
                Accessible.name: section
            }

            delegate: ItemDelegate {
                id: row
                required property int index
                required property string name
                required property string detail
                required property string warn
                required property string badge
                required property string profile
                required property bool saved

                readonly property bool current: ListView.isCurrentItem
                readonly property bool strong: current && (list.activeFocus || search.activeFocus)

                width: ListView.view.width - 16
                // A finger needs 44 px whatever the font makes of the text.
                implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding, Style.touch ? 44 : 0)
                topPadding: 7
                bottomPadding: 7
                leftPadding: 8
                rightPadding: 8
                hoverEnabled: true
                // A finger taps once to connect and holds for the menu; a
                // mouse selects, double-clicks, and right-clicks.
                onClicked: {
                    list.currentIndex = index
                    if (Style.touch) {
                        side.connectRequested(JSON.parse(profile))
                        return
                    }
                    list.forceActiveFocus()
                }
                onDoubleClicked: if (!Style.touch) side.connectRequested(JSON.parse(profile))
                onPressAndHold: {
                    if (!Style.touch)
                        return
                    list.currentIndex = index
                    menu.popup(row, Math.min(row.width - 60, 48), row.height - 6)
                }

                Accessible.name: warn ? name + ", " + detail + ", " + warn : name + ", " + detail

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        list.currentIndex = row.index
                        menu.popup()
                    }
                }

                background: Rectangle {
                    radius: 7
                    color: row.strong ? Style.accent
                         : row.current ? Style.selection
                         : row.hovered ? Qt.rgba(1, 1, 1, 0.04) : "transparent"
                }

                contentItem: RowLayout {
                    spacing: 10
                    Rectangle {
                        Layout.preferredWidth: 26
                        Layout.preferredHeight: 26
                        radius: 7
                        gradient: Gradient {
                            GradientStop { position: 0; color: Qt.lighter(side.badgeColor(row.badge), 1.15) }
                            GradientStop { position: 1; color: side.badgeColor(row.badge) }
                        }
                        border.width: row.strong ? 1 : 0
                        border.color: Style.accentTextSecondary
                        Icon {
                            anchors.centerIn: parent
                            name: side.badgeIcon(row.badge)
                            size: 15
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Text {
                            objectName: "hostName"
                            Layout.fillWidth: true
                            text: row.name
                            elide: Text.ElideRight
                            // At the row's start whatever the name is written in.
                            horizontalAlignment: Text.AlignLeft
                            font.family: Style.fontFamily
                            font.pixelSize: Style.fontSize
                            font.weight: Font.Medium
                            color: row.strong ? Style.accentText : Style.label
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Icon {
                                visible: row.warn !== ""
                                name: "warning"
                                size: 11
                                color: row.strong ? Style.accentText : "white"
                                opacity: 0.8
                            }
                            Text {
                                Layout.fillWidth: true
                                text: row.warn ? row.warn + "  ·  " + row.detail : row.detail
                                elide: Text.ElideMiddle
                                horizontalAlignment: Text.AlignLeft
                                font.family: Style.fontFamily
                                font.pixelSize: Style.smallFontSize
                                color: row.strong ? Style.accentTextSecondary
                                     : row.warn ? Style.orange : Style.secondaryLabel
                            }
                        }
                    }
                }
            }

            Keys.onReturnPressed: side.connectCurrent()
            Keys.onEnterPressed: side.connectCurrent()
            Keys.onDeletePressed: side.deleteCurrent()
            Keys.onEscapePressed: side.leaveRequested()
            Keys.onPressed: event => {
                if (event.key === Qt.Key_E && event.modifiers === Qt.NoModifier) {
                    side.editCurrent()
                    event.accepted = true
                } else if (event.text.length === 1 && event.text.trim() !== "" && event.modifiers <= Qt.ShiftModifier) {
                    // Typing in the list goes on typing into the search.
                    search.forceActiveFocus()
                    search.text += event.text
                    event.accepted = true
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 48
                visible: items.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                font.family: Style.fontFamily
                font.pixelSize: 12
                color: Style.secondaryLabel
                text: qsTr("Nothing matches. Type user@host or a /dev path to connect anyway.")
            }
        }

        // A newer Tether, or one waiting for a restart.
        Rectangle {
            id: updateBanner
            visible: updater.available || side.updateInstalling
            Layout.fillWidth: true
            Layout.margins: 8
            Layout.bottomMargin: 0
            implicitHeight: Style.touch ? 48 : 40
            radius: 8
            color: updateArea.containsMouse ? Style.tint(0.28) : Style.tint(0.2)
            border.color: Style.tint(0.45)
            activeFocusOnTab: true
            Keys.onReturnPressed: side.updateRequested()
            Keys.onEnterPressed: side.updateRequested()
            Keys.onSpacePressed: side.updateRequested()
            Accessible.role: Accessible.Button
            Accessible.name: side.updateInstalling ? qsTr("Restart to finish updating")
                                                   : qsTr("Tether %1 is available").arg(updater.latestVersion)
            Accessible.onPressAction: side.updateRequested()
            TFocusRing { visible: updateBanner.activeFocus }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 8
                Icon { name: side.updateInstalling ? "reload" : "download"; size: 15 }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        font.family: Style.fontFamily
                        font.pixelSize: Style.fontSize - 1
                        font.weight: Font.DemiBold
                        color: Style.label
                        text: side.updateInstalling ? qsTr("Restart to finish updating")
                                                    : qsTr("Tether %1 is available").arg(updater.latestVersion)
                    }
                    Text {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        font.family: Style.fontFamily
                        font.pixelSize: Style.smallFontSize
                        color: Style.secondaryLabel
                        text: side.updateInstalling ? (Style.touch ? qsTr("Tap when the install tab is done")
                                                                   : qsTr("Click when the install tab is done"))
                                                    : Style.touch ? qsTr("Tap to see what's new") : qsTr("Click to see what's new")
                    }
                }
            }
            MouseArea {
                id: updateArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: side.updateRequested()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 8
            implicitHeight: 1
            color: Style.hairline
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 8
            spacing: 2
            TButton {
                kind: "toolbar"
                iconName: "plus"
                checked: addMenu.visible
                label: addMenu.visible ? "" : qsTr("New host or serial port")
                Accessible.name: qsTr("New host or serial port")
                onClicked: addMenu.popup(this, 0, -addMenu.implicitHeight - 4)
            }
            TButton {
                kind: "toolbar"
                iconName: "snippets"
                text: qsTr("Snippets")
                onClicked: side.snippetsRequested()
            }
            Item { Layout.fillWidth: true }
            TButton {
                kind: "toolbar"
                iconName: "gear"
                iconSize: 17
                label: qsTr("Settings")
                onClicked: side.settingsRequested()
            }
            TButton {
                visible: !Style.touch
                kind: "toolbar"
                iconName: "keyboard"
                iconSize: 17
                label: qsTr("Keyboard shortcuts")
                onClicked: side.shortcutsRequested()
            }
        }
    }

    TMenu {
        id: addMenu
        TMenuItem { text: qsTr("New host…"); onTriggered: side.newRequested("ssh") }
        TMenuItem { text: qsTr("New serial port…"); onTriggered: side.newRequested("serial") }
    }

    // For tests: whether the host menu is showing.
    readonly property alias menuOpen: menu.visible

    TMenu {
        id: menu
        TMenuItem { text: qsTr("Connect"); onTriggered: side.connectCurrent() }
        TMenuItem {
            readonly property var row: side.current()
            visible: row !== null && JSON.parse(row.profile).kind === "ssh" && backend.sftpAvailable
            height: visible ? implicitHeight : 0
            text: qsTr("Browse files")
            onTriggered: side.filesRequested(JSON.parse(side.current().profile))
        }
        TSeparator {}
        TMenuItem {
            readonly property var row: side.current()
            text: row && row.saved ? qsTr("Edit…") : qsTr("Save as…")
            enabled: row !== null && ["local", "tmux"].indexOf(JSON.parse(row.profile).kind) < 0
            onTriggered: side.editCurrent()
        }
        TMenuItem {
            text: qsTr("Delete…")
            enabled: side.current() !== null && side.current().saved
            onTriggered: side.deleteCurrent()
        }
    }
}
