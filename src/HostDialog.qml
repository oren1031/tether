import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Tether

// Adds or edits a saved SSH host or serial port. Passwords are never stored:
// ssh asks for them in the terminal, or uses keys and ssh-agent.
TSheet {
    id: dialog

    property string profileId: ""
    property string kind: "ssh"
    property string error: ""
    property string transport: "ssh"
    property var startupIds: []

    large: true
    width: preferredWidth(560)
    height: preferredHeight(700)
    title: profileId ? qsTr("Edit %1").arg(name.text) : kind === "ssh" ? qsTr("New host") : qsTr("New serial port")
    subtitle: kind === "ssh" ? qsTr("To sign in without typing a password, save it as a login in Settings → Vault and pick it below.") : ""
    padding: 0

    readonly property var bauds: ["300", "1200", "2400", "4800", "9600", "19200", "38400", "57600",
                                  "115200", "230400", "460800", "921600", "1000000", "1500000", "2000000"]

    function choose(combo, value) {
        const i = combo.indexOfValue(value)
        combo.currentIndex = i >= 0 ? i : 0
    }

    function openFor(profile) {
        const p = profile || {}
        kind = p.kind || "ssh"
        profileId = p.id || ""
        error = ""
        name.text = p.name || ""
        host.text = p.host || ""
        user.text = p.user || ""
        port.text = String(p.port || 22)
        identity.text = p.identityFile || ""
        jump.text = p.jumpHost || ""
        options.text = p.options || ""
        forwards.clear()
        for (const f of (p.forwards || []))
            forwards.append({ type: f.type, spec: f.spec })
        device.editText = p.device || (serialPorts.devices.length ? serialPorts.devices[0].stablePath : backend.exampleDevice)
        baud.editText = String(p.baud || 115200)
        choose(dataBits, p.dataBits || 8)
        choose(parity, p.parity || "none")
        choose(stopBits, p.stopBits || 1)
        choose(flow, p.flow || "none")
        choose(enter, p.enter || "cr")
        echo.checked = !!p.echo
        log.checked = !!p.log
        transport = p.transport || "ssh"
        persistent.checked = !!p.persistent
        reconnect.checked = p.reconnect === undefined ? true : !!p.reconnect
        startupIds = p.startupSnippets || []
        const ids = [""].concat(backend.identities.map(i => i.id))
        login.currentIndex = Math.max(0, ids.indexOf(p.identityId || ""))
        open()
        name.forceActiveFocus()
    }

    function profile() {
        const p = { id: profileId, kind: kind, name: name.text, log: log.checked, startupSnippets: startupIds }
        if (kind === "ssh") {
            Object.assign(p, { host: host.text, user: user.text, port: parseInt(port.text) || 0,
                               identityFile: identity.text, jumpHost: jump.text, options: options.text, forwards: [],
                               identityId: login.currentIndex > 0 ? backend.identities[login.currentIndex - 1].id : "",
                               transport: transport, persistent: persistent.checked, reconnect: reconnect.checked })
            for (let i = 0; i < forwards.count; ++i)
                p.forwards.push({ type: forwards.get(i).type, spec: forwards.get(i).spec })
        } else {
            Object.assign(p, { device: device.editText, baud: parseInt(baud.editText) || 0,
                               dataBits: dataBits.currentValue, parity: parity.currentValue,
                               stopBits: stopBits.currentValue, flow: flow.currentValue,
                               enter: enter.currentValue, echo: echo.checked })
        }
        return p
    }

    function save() {
        error = backend.saveProfile(profile())
        if (!error)
            close()
        else
            focusFault()
    }

    // Puts the cursor in the field the error is about: the first required
    // one left empty, or the one holding the value the error quotes.
    function focusFault() {
        const quoted = /“([^”]*)”/.exec(error)
        const value = quoted ? quoted[1] : null
        let field = null
        if (!name.text.trim())
            field = name
        else if (kind === "ssh") {
            if (!host.text.trim() || value === host.text.trim())
                field = host
            else if (value !== null && value === user.text.trim())
                field = user
            else if (value !== null && value === jump.text.trim())
                field = jump
            else if (value !== null && options.text.split("\n").some(line => line.trim() === value))
                field = options
            else if (!(parseInt(port.text) >= 1 && parseInt(port.text) <= 65535))
                field = port
        } else if (!device.editText.trim()) {
            field = device
        } else if (!(parseInt(baud.editText) >= 50)) {
            field = baud
        }
        if (field) {
            field.forceActiveFocus()
            if (field.selectAll)
                field.selectAll()
        }
    }

    ListModel { id: forwards }

    // A key file from anywhere; on Android it's copied into Tether's storage.
    FileDialog {
        id: keyPicker
        title: qsTr("Choose a private key")
        onAccepted: {
            const result = backend.importKey(selectedFile)
            dialog.error = result.error || ""
            if (!result.error)
                identity.text = result.path
        }
    }

    // A right-aligned label beside its control, as in a macOS form.
    // Beside its field on a desktop, above it on a phone.
    component FormLabel: Text {
        Layout.alignment: Style.compact ? Qt.AlignLeft | Qt.AlignBottom : Qt.AlignRight | Qt.AlignVCenter
        Layout.preferredWidth: Style.compact ? -1 : 110
        Layout.topMargin: Style.compact ? 4 : 0
        horizontalAlignment: Style.compact ? Text.AlignLeft : Text.AlignRight
        font.family: Style.fontFamily
        font.pixelSize: Style.fontSize
        color: Style.secondaryLabel
        elide: Text.ElideRight
    }
    // Holds the label column's place on a desktop; nothing on a phone.
    component Spacer: Item {
        visible: !Style.compact
        Layout.preferredWidth: 110
    }
    component Hint: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.family: Style.fontFamily
        font.pixelSize: Style.smallFontSize
        color: Style.tertiaryLabel
    }
    component Group: Rectangle {
        Layout.fillWidth: true
        implicitHeight: groupGrid.implicitHeight + 24
        radius: 10
        color: Qt.rgba(1, 1, 1, 0.035)
        border.color: Qt.rgba(1, 1, 1, 0.06)
        default property alias content: groupGrid.data
        GridLayout {
            id: groupGrid
            anchors.fill: parent
            anchors.margins: 12
            columns: Style.compact ? 1 : 2
            columnSpacing: 12
            rowSpacing: Style.compact ? 6 : 10
        }
    }

    contentItem: ScrollView {
        id: scroll
        clip: true
        contentWidth: availableWidth
        ScrollBar.vertical: TScrollBar { parent: scroll; x: scroll.mirrored ? 0 : scroll.width - width; height: scroll.height }

        ColumnLayout {
            x: 20
            width: scroll.availableWidth - 40
            spacing: 14

            Group {
                FormLabel { text: qsTr("Name") }
                TField {
                    id: name
                    objectName: "nameField"
                    Layout.fillWidth: true
                    label: qsTr("Name")
                    placeholderText: dialog.kind === "ssh" ? qsTr("web-01") : qsTr("ESP32 console")
                    onAccepted: dialog.save()
                }

                // SSH
                FormLabel { visible: dialog.kind === "ssh"; text: qsTr("Host") }
                RowLayout {
                    visible: dialog.kind === "ssh"
                    Layout.fillWidth: true
                    spacing: 8
                    TField {
                        id: host
                        objectName: "hostField"
                        Layout.fillWidth: true
                        label: qsTr("Host")
                        placeholderText: qsTr("Host or address")
                        onAccepted: dialog.save()
                    }
                    TField {
                        id: port
                        Layout.preferredWidth: 72
                        label: qsTr("Port")
                        placeholderText: qsTr("Port")
                        horizontalAlignment: TextInput.AlignHCenter
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator { bottom: 1; top: 65535 }
                        onAccepted: dialog.save()
                    }
                }
                FormLabel { visible: dialog.kind === "ssh"; text: qsTr("User") }
                TField {
                    id: user
                    visible: dialog.kind === "ssh"
                    Layout.fillWidth: true
                    label: qsTr("User")
                    placeholderText: login.currentIndex > 0 ? qsTr("From the login: %1").arg(backend.identities[login.currentIndex - 1].user || qsTr("none"))
                                                            : Style.touch ? qsTr("Your user on the server") : qsTr("From ~/.ssh/config or your login")
                    onAccepted: dialog.save()
                }
                FormLabel { visible: dialog.kind === "ssh"; text: qsTr("Login") }
                TCombo {
                    id: login
                    visible: dialog.kind === "ssh"
                    Layout.fillWidth: true
                    label: qsTr("Login")
                    model: [qsTr("None (ssh asks, or uses your keys)")].concat(backend.identities.map(i => i.name + (i.user ? " (" + i.user + ")" : "")))
                }

                // Serial
                FormLabel { visible: dialog.kind === "serial"; text: qsTr("Device") }
                TCombo {
                    id: device
                    visible: dialog.kind === "serial"
                    Layout.fillWidth: true
                    label: qsTr("Device")
                    editable: true
                    model: serialPorts.devices.map(d => d.stablePath)
                }
                FormLabel { visible: dialog.kind === "serial"; text: qsTr("Baud rate") }
                TCombo {
                    id: baud
                    visible: dialog.kind === "serial"
                    Layout.fillWidth: true
                    label: qsTr("Baud rate")
                    editable: true
                    model: dialog.bauds
                    validator: IntValidator { bottom: 50; top: 16000000 }
                }
            }

            Group {
                visible: dialog.kind === "ssh"
                // Only where mosh could run.
                FormLabel { visible: !Style.touch || backend.moshAvailable; text: qsTr("Connect with") }
                RowLayout {
                    visible: !Style.touch || backend.moshAvailable
                    Layout.fillWidth: true
                    spacing: 10
                    TSegmented {
                        label: qsTr("Connect with")
                        model: [{ value: "ssh", text: "ssh" }, { value: "mosh", text: "mosh" }]
                        value: dialog.transport
                        onActivated: v => dialog.transport = v
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        font.family: Style.fontFamily
                        font.pixelSize: Style.smallFontSize
                        color: dialog.transport === "mosh" && !backend.moshAvailable ? Style.orange : Style.tertiaryLabel
                        text: dialog.transport === "mosh"
                              ? (backend.moshAvailable ? qsTr("Survives sleep and network changes. Needs mosh on the server too.")
                                                       : qsTr("mosh isn't installed here. Install the mosh package."))
                              : qsTr("Works everywhere.")
                    }
                }
                Spacer {}
                TCheck {
                    id: persistent
                    objectName: "keepRunning"
                    Layout.fillWidth: true
                    text: qsTr("Keep the session running on the server (tmux)")
                }
                // Hidden along with the note it sits beside, or the grid
                // below would shift by a cell.
                Spacer { visible: !Style.compact && persistent.checked }
                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin: 26
                    Layout.topMargin: Style.compact ? 0 : -8
                    visible: persistent.checked
                    horizontalAlignment: Text.AlignLeft
                    wrapMode: Text.Wrap
                    font.family: Style.fontFamily
                    font.pixelSize: Style.smallFontSize
                    color: Style.tertiaryLabel
                    text: qsTr("Programs keep running if the connection drops or Tether closes; connecting again picks up where you left off. Uses tmux on the server when it's there.")
                }
                Spacer { visible: !Style.compact && dialog.transport === "ssh" }
                TCheck {
                    id: reconnect
                    objectName: "reconnect"
                    visible: dialog.transport === "ssh"
                    Layout.fillWidth: true
                    text: qsTr("Reconnect when the connection drops")
                }
            }

            Group {
                visible: backend.snippets.length > 0
                FormLabel {
                    Layout.alignment: (Style.compact ? Qt.AlignLeft : Qt.AlignRight) | Qt.AlignTop
                    Layout.topMargin: 2
                    text: qsTr("Run on connect")
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Repeater {
                        model: backend.snippets
                        delegate: TCheck {
                            required property var modelData
                            Layout.fillWidth: true
                            text: modelData.name
                            checked: dialog.startupIds.indexOf(modelData.id) >= 0
                            onToggled: {
                                const ids = dialog.startupIds.filter(id => id !== modelData.id)
                                if (checked)
                                    ids.push(modelData.id)
                                dialog.startupIds = ids
                            }
                        }
                    }
                    Hint { text: qsTr("Typed in order once you're logged in.") }
                }
            }

            Group {
                visible: dialog.kind === "ssh"
                FormLabel { text: qsTr("Key file") }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    TField {
                        id: identity
                        Layout.fillWidth: true
                        label: qsTr("Key file")
                        placeholderText: qsTr("~/.ssh/id_ed25519 (optional)")
                        onAccepted: dialog.save()
                    }
                    TButton {
                        text: qsTr("Choose…")
                        onClicked: keyPicker.open()
                    }
                }
                FormLabel { text: qsTr("Jump host") }
                TField {
                    id: jump
                    Layout.fillWidth: true
                    label: qsTr("Jump host")
                    placeholderText: qsTr("user@bastion (optional)")
                    onAccepted: dialog.save()
                }
                FormLabel {
                    Layout.alignment: (Style.compact ? Qt.AlignLeft : Qt.AlignRight) | Qt.AlignTop
                    Layout.topMargin: 8
                    text: qsTr("ssh options")
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    TTextArea {
                        id: options
                        Layout.fillWidth: true
                        label: qsTr("ssh options")
                        Layout.minimumHeight: 64
                        placeholderText: "ServerAliveInterval=30"
                    }
                    Hint { text: qsTr("Extra ssh options, one per line") }
                }
            }

            Group {
                visible: dialog.kind === "ssh"
                FormLabel {
                    Layout.alignment: (Style.compact ? Qt.AlignLeft : Qt.AlignRight) | Qt.AlignTop
                    Layout.topMargin: 6
                    text: qsTr("Port forwarding")
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Repeater {
                        model: dialog.kind === "ssh" ? forwards : null
                        delegate: RowLayout {
                            id: forwardRow
                            required property int index
                            required property string type
                            required property string spec
                            Layout.fillWidth: true
                            spacing: 6
                            TCombo {
                                Layout.preferredWidth: 104
                                label: qsTr("Kind of forward")
                                textRole: "text"
                                valueRole: "value"
                                model: [{ value: "L", text: qsTr("Local") }, { value: "R", text: qsTr("Remote") },
                                        { value: "D", text: qsTr("SOCKS") }]
                                Component.onCompleted: currentIndex = indexOfValue(forwardRow.type)
                                onActivated: forwards.setProperty(forwardRow.index, "type", currentValue)
                            }
                            TField {
                                Layout.fillWidth: true
                                text: forwardRow.spec
                                label: qsTr("Forward")
                                font.family: Style.monoFamily
                                font.pixelSize: 12
                                placeholderText: forwardRow.type === "D" ? qsTr("Port, like 1080") : qsTr("8080:localhost:80")
                                onTextEdited: forwards.setProperty(forwardRow.index, "spec", text)
                            }
                            TButton {
                                kind: "toolbar"
                                iconName: "close"
                                iconSize: 12
                                onClicked: forwards.remove(forwardRow.index)
                                label: qsTr("Remove this forward")
                            }
                        }
                    }
                    // In a row with room to spare: a column holding only a
                    // button can't grow, and the label column would take up
                    // the slack and drift toward the middle.
                    RowLayout {
                        Layout.fillWidth: true
                        TButton {
                            kind: "plain"
                            leftPadding: 0
                            text: qsTr("Add forward")
                            onClicked: forwards.append({ type: "L", spec: "" })
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            Group {
                visible: dialog.kind === "serial"
                FormLabel { text: qsTr("Data bits") }
                TCombo {
                    id: dataBits
                    Layout.fillWidth: true
                    label: qsTr("Data bits")
                    textRole: "text"
                    valueRole: "value"
                    model: [{ value: 8, text: "8" }, { value: 7, text: "7" }, { value: 6, text: "6" }, { value: 5, text: "5" }]
                }
                FormLabel { text: qsTr("Parity") }
                TCombo {
                    id: parity
                    Layout.fillWidth: true
                    label: qsTr("Parity")
                    textRole: "text"
                    valueRole: "value"
                    model: [{ value: "none", text: qsTr("None") }, { value: "even", text: qsTr("Even") },
                            { value: "odd", text: qsTr("Odd") }]
                }
                FormLabel { text: qsTr("Stop bits") }
                TCombo {
                    id: stopBits
                    Layout.fillWidth: true
                    label: qsTr("Stop bits")
                    textRole: "text"
                    valueRole: "value"
                    model: [{ value: 1, text: "1" }, { value: 2, text: "2" }]
                }
                FormLabel { text: qsTr("Flow control") }
                TCombo {
                    id: flow
                    Layout.fillWidth: true
                    label: qsTr("Flow control")
                    textRole: "text"
                    valueRole: "value"
                    model: [{ value: "none", text: qsTr("None") }, { value: "hardware", text: qsTr("Hardware (RTS/CTS)") },
                            { value: "software", text: qsTr("Software (XON/XOFF)") }]
                }
                FormLabel { text: qsTr("Enter sends") }
                TCombo {
                    id: enter
                    Layout.fillWidth: true
                    label: qsTr("Enter sends")
                    textRole: "text"
                    valueRole: "value"
                    model: [{ value: "cr", text: "CR" }, { value: "lf", text: "LF" }, { value: "crlf", text: "CR LF" }]
                }
                Spacer {}
                TCheck {
                    id: echo
                    Layout.fillWidth: true
                    text: qsTr("Show what I type (local echo)")
                }
            }

            Group {
                Spacer {}
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    TCheck {
                        id: log
                        Layout.fillWidth: true
                        text: qsTr("Save everything this session prints to a log file")
                    }
                    Hint {
                        visible: log.checked
                        leftPadding: 26
                        text: qsTr("Logs go to %1").arg(backend.logDirectory)
                    }
                }
            }

            Item { implicitHeight: 2 }
        }
    }

    // What's wrong stays in view beside the buttons, however far the form
    // is scrolled.
    footer: Rectangle {
        implicitHeight: Math.max(60, faultText.implicitHeight + 24)
        color: "transparent"
        Rectangle { width: parent.width; height: 1; color: Style.hairline }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            spacing: 10
            Icon {
                visible: dialog.error !== ""
                name: "warning"
                size: 14
                color: Style.errorText
            }
            Text {
                id: faultText
                objectName: "hostError"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignLeft
                font.family: Style.fontFamily
                font.pixelSize: Style.fontSize
                color: Style.errorText
                text: dialog.error
                Accessible.role: Accessible.AlertMessage
                Accessible.name: text
            }
            TButton {
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
            TButton {
                objectName: "saveHost"
                kind: "primary"
                text: qsTr("Save")
                onClicked: dialog.save()
            }
        }
    }
}
