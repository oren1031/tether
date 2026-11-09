import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Tether

// A host's files over sftp, as a pane beside its terminals: browse, upload
// by dropping files in or picking them, download, rename, and delete.
// It shares the terminal's login, so it needs no password of its own.
FocusScope {
    id: pane

    readonly property bool isPane: true
    property var container: null
    property var page: null
    property var launch: ({})
    property bool ended: false
    readonly property var terminal: null
    readonly property var profile: launch.profile || ({})
    readonly property string title: qsTr("Files on %1").arg(profile.name || profile.host || "")
    readonly property bool active: page !== null && page.activePane === pane
    readonly property bool split: page !== null && page.paneCount > 1
    readonly property alias browser: browser

    signal reconnectRequested()

    function focusTerminal() {
        if (page)
            page.activePane = pane
        list.forceActiveFocus()
    }
    function send(text) {}
    function restart(newLaunch) {
        launch = newLaunch
        browser.start(backend.sftpCommand(profile))
    }

    function size(bytes) {
        if (bytes < 1024)
            return qsTr("%1 bytes").arg(bytes)
        const units = ["KB", "MB", "GB", "TB"]
        let n = bytes / 1024, u = 0
        while (n >= 1024 && u < units.length - 1) {
            n /= 1024
            ++u
        }
        return n.toFixed(n < 10 ? 1 : 0) + " " + units[u]
    }

    function current() {
        return list.currentIndex >= 0 ? browser.entries[list.currentIndex] : null
    }

    function activate(entry) {
        if (!entry)
            return
        if (entry.folder || entry.link)
            browser.open(entry.name)
        else
            browser.download([entry.name])
    }

    SftpBrowser {
        id: browser
        onDownloaded: (folder, names) => {
            toast.text = names.length === 1 ? qsTr("Downloaded %1").arg(names[0]) : qsTr("Downloaded %n items", "", names.length)
            toast.folder = folder
            toast.show()
        }
    }

    Component.onCompleted: browser.start(backend.sftpCommand(profile))

    Rectangle {
        anchors.fill: parent
        color: Style.window
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Toolbar: up, the path as a breadcrumb, and actions.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Style.touch ? 48 : 40
            color: Style.toolbar
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Style.separator }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 2

                TButton {
                    kind: "toolbar"
                    iconName: "arrow-up"
                    enabled: browser.path !== "/" && browser.path !== "" && !browser.busy
                    label: qsTr("Enclosing folder")
                    onClicked: browser.up()
                }

                Flickable {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: crumbs.implicitWidth
                    clip: true
                    interactive: contentWidth > width
                    // Keeps the deepest folder in view.
                    contentX: Math.max(0, contentWidth - width)
                    Row {
                        id: crumbs
                        height: parent.height
                        spacing: 0
                        Repeater {
                            model: {
                                const parts = browser.path.split("/").filter(p => p)
                                const out = [{ name: "/", path: "/" }]
                                let acc = ""
                                for (const p of parts) {
                                    acc += "/" + p
                                    out.push({ name: p, path: acc })
                                }
                                return out
                            }
                            delegate: Row {
                                required property int index
                                required property var modelData
                                anchors.verticalCenter: parent.verticalCenter
                                Icon {
                                    visible: index > 0
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: "chevron-right"
                                    size: 11
                                    opacity: 0.5
                                    mirror: Style.mirrored
                                }
                                Rectangle {
                                    id: crumb
                                    width: Math.max(Style.touch ? 44 : 0, crumbText.implicitWidth + 12)
                                    height: Style.touch ? 40 : 24
                                    radius: 5
                                    color: crumbArea.containsMouse ? Style.fill : "transparent"
                                    activeFocusOnTab: true
                                    Keys.onReturnPressed: browser.go(modelData.path)
                                    Keys.onEnterPressed: browser.go(modelData.path)
                                    Keys.onSpacePressed: browser.go(modelData.path)
                                    Accessible.role: Accessible.Button
                                    Accessible.name: modelData.path === "/" ? qsTr("Top folder") : modelData.name
                                    Accessible.onPressAction: browser.go(modelData.path)
                                    TFocusRing { gap: 1; visible: crumb.activeFocus }
                                    Text {
                                        id: crumbText
                                        anchors.centerIn: parent
                                        text: modelData.name
                                        font.family: Style.fontFamily
                                        font.pixelSize: Style.fontSize - 1
                                        font.weight: modelData.path === browser.path ? Font.DemiBold : Font.Normal
                                        color: modelData.path === browser.path ? Style.label : Style.secondaryLabel
                                    }
                                    MouseArea {
                                        id: crumbArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: browser.go(modelData.path)
                                    }
                                }
                            }
                        }
                    }
                }

                TButton {
                    kind: "toolbar"
                    iconName: "upload"
                    enabled: !browser.busy && browser.path !== ""
                    label: qsTr("Upload files here")
                    onClicked: picker.open()
                }
                TButton {
                    kind: "toolbar"
                    iconName: "folder-plus"
                    enabled: !browser.busy && browser.path !== ""
                    label: qsTr("New folder")
                    onClicked: prompt.ask(qsTr("New folder"), qsTr("In %1").arg(browser.path),
                                          [{ key: "name", label: "", text: qsTr("untitled folder") }],
                                          qsTr("Create"), v => browser.makeFolder(v.name))
                }
                TButton {
                    kind: "toolbar"
                    iconName: browser.busy ? "close" : "reload"
                    label: browser.busy ? qsTr("Stop") : qsTr("Refresh")
                    onClicked: browser.busy ? browser.cancel() : browser.refresh()
                }
            }
        }

        // Column headings.
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            Layout.topMargin: 8
            Layout.bottomMargin: 4
            spacing: 12
            component Heading: Text {
                horizontalAlignment: Text.AlignLeft
                font.family: Style.fontFamily
                font.pixelSize: Style.smallFontSize
                font.weight: Font.DemiBold
                color: Style.tertiaryLabel
            }
            Heading { Layout.fillWidth: true; text: qsTr("Name") }
            Heading { Layout.preferredWidth: 80; horizontalAlignment: Text.AlignRight; text: qsTr("Size") }
            Heading { Layout.preferredWidth: 110; text: qsTr("Modified"); visible: pane.width > 460 }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            leftMargin: 8
            rightMargin: 8
            clip: true
            model: browser.entries
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            focus: true
            ScrollBar.vertical: TScrollBar {}
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Files in %1").arg(browser.path)

            delegate: ItemDelegate {
                id: row
                required property int index
                required property var modelData
                readonly property bool current: ListView.isCurrentItem
                readonly property bool strong: current && list.activeFocus
                width: ListView.view.width - 16
                height: Style.touch ? 44 : 30
                leftPadding: 10
                rightPadding: 10
                hoverEnabled: true
                background: Rectangle {
                    radius: 6
                    color: row.strong ? Style.accent : row.current ? Style.selection
                         : row.index % 2 ? Qt.rgba(1, 1, 1, 0.02) : "transparent"
                }
                // A finger taps once to open or download and holds for the
                // menu; a mouse selects, double-clicks, and right-clicks.
                onClicked: {
                    list.currentIndex = index
                    list.forceActiveFocus()
                    if (pane.page)
                        pane.page.activePane = pane
                    if (Style.touch)
                        pane.activate(modelData)
                }
                onDoubleClicked: if (!Style.touch) pane.activate(modelData)
                onPressAndHold: {
                    if (!Style.touch)
                        return
                    list.currentIndex = index
                    fileMenu.popup(row, Math.min(row.width - 60, 48), row.height - 6)
                }
                Accessible.name: modelData.folder ? qsTr("%1, folder").arg(modelData.name)
                                                  : modelData.name + ", " + pane.size(modelData.size)
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: { list.currentIndex = row.index; fileMenu.popup() }
                }
                contentItem: RowLayout {
                    spacing: 12
                    Row {
                        Layout.fillWidth: true
                        spacing: 8
                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: row.modelData.folder ? "folder" : row.modelData.link ? "link" : "file"
                            size: 15
                            color: row.strong ? Style.accentText : "white"
                            opacity: row.modelData.folder ? 0.9 : 0.6
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 23
                            text: row.modelData.name
                            elide: Text.ElideMiddle
                            horizontalAlignment: Text.AlignLeft
                            font.family: Style.fontFamily
                            font.pixelSize: Style.fontSize
                            color: row.strong ? Style.accentText : Style.label
                        }
                    }
                    Text {
                        Layout.preferredWidth: 80
                        horizontalAlignment: Text.AlignRight
                        text: row.modelData.folder ? "—" : pane.size(row.modelData.size)
                        font.family: Style.fontFamily
                        font.pixelSize: Style.smallFontSize
                        font.features: { "tnum": 1 }
                        color: row.strong ? Style.accentTextSecondary : Style.secondaryLabel
                    }
                    Text {
                        visible: pane.width > 460
                        Layout.preferredWidth: 110
                        text: row.modelData.modified
                        font.family: Style.fontFamily
                        font.pixelSize: Style.smallFontSize
                        color: row.strong ? Style.accentTextSecondary : Style.secondaryLabel
                    }
                }
            }

            Keys.onReturnPressed: pane.activate(pane.current())
            Keys.onEnterPressed: pane.activate(pane.current())
            Keys.onBackPressed: browser.up()
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Backspace) {
                    browser.up()
                    event.accepted = true
                } else if (event.key === Qt.Key_Delete && pane.current()) {
                    deleteCurrent()
                    event.accepted = true
                } else if (event.key === Qt.Key_F2 && pane.current()) {
                    renameCurrent()
                    event.accepted = true
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 60
                visible: browser.entries.length === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                font.family: Style.fontFamily
                font.pixelSize: Style.fontSize
                color: browser.error ? Style.errorText : Style.secondaryLabel
                text: browser.error ? browser.error
                    : browser.busy ? browser.status
                    : qsTr("This folder is empty. Drop files here to upload them.")
            }
        }

        // Status line.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 28
            color: Style.toolbar
            Rectangle { width: parent.width; height: 1; color: Style.hairline }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8
                Rectangle {
                    visible: browser.busy
                    width: 8
                    height: 8
                    radius: 4
                    color: Style.accent
                    SequentialAnimation on opacity {
                        running: browser.busy
                        loops: Animation.Infinite
                        NumberAnimation { from: 1; to: 0.25; duration: 500 }
                        NumberAnimation { from: 0.25; to: 1; duration: 500 }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    font.family: Style.fontFamily
                    font.pixelSize: Style.smallFontSize
                    color: browser.error && browser.entries.length ? Style.errorText : Style.secondaryLabel
                    text: browser.busy ? browser.status
                        : browser.error && browser.entries.length ? browser.error
                        : qsTr("%n items", "", browser.entries.length) + "  ·  " + qsTr("Downloads go to %1").arg(browser.downloadFolder)
                }
            }
        }
    }

    // Files dropped from the file manager upload into the open folder.
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: drop => {
            if (drop.hasUrls) {
                browser.upload(drop.urls)
                drop.accept(Qt.CopyAction)
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 6
            visible: parent.containsDrag
            radius: 10
            color: Style.tint(0.12)
            border.width: 2
            border.color: Style.accent
            Text {
                anchors.centerIn: parent
                text: qsTr("Drop to upload to %1").arg(browser.path)
                font.family: Style.fontFamily
                font.pixelSize: Style.titleFontSize
                font.weight: Font.DemiBold
                color: Style.label
            }
        }
    }

    // Marks the focused pane, as terminals do.
    Rectangle {
        anchors.fill: parent
        anchors.margins: 1
        color: "transparent"
        radius: 6
        border.width: 1
        border.color: Style.tint(0.7)
        opacity: pane.split && pane.active ? 1 : 0
    }

    // A note that a download finished, with a way to open its folder.
    Rectangle {
        id: toast
        property string text
        property string folder
        function show() { opacity = 1; hide.restart() }
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 40
        width: toastRow.implicitWidth + 24
        height: 36
        radius: 10
        color: Style.popover
        border.color: Qt.rgba(1, 1, 1, 0.12)
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Style.normal } }
        Timer { id: hide; interval: 5000; onTriggered: toast.opacity = 0 }
        RowLayout {
            id: toastRow
            anchors.centerIn: parent
            spacing: 10
            Icon { name: "download"; size: 14; opacity: 0.8 }
            Text {
                text: toast.text
                font.family: Style.fontFamily
                font.pixelSize: Style.fontSize - 1
                color: Style.label
            }
            TButton {
                kind: "plain"
                leftPadding: 0
                rightPadding: 0
                text: qsTr("Show")
                onClicked: Qt.openUrlExternally("file://" + toast.folder)
            }
        }
    }

    function renameCurrent() {
        const entry = current()
        if (entry)
            prompt.ask(qsTr("Rename"), "", [{ key: "name", label: "", text: entry.name }], qsTr("Rename"),
                       v => browser.rename(entry.name, v.name))
    }

    function deleteCurrent() {
        const entry = current()
        if (!entry)
            return
        confirm.target = entry
        confirm.open()
    }

    TMenu {
        id: fileMenu
        TMenuItem {
            text: pane.current() && pane.current().folder ? qsTr("Open") : qsTr("Download")
            onTriggered: pane.activate(pane.current())
        }
        TMenuItem {
            text: qsTr("Download")
            visible: pane.current() !== null && pane.current().folder
            height: visible ? implicitHeight : 0
            onTriggered: browser.download([pane.current().name])
        }
        TSeparator {}
        TMenuItem { text: qsTr("Rename…"); onTriggered: pane.renameCurrent() }
        TMenuItem { text: qsTr("Delete…"); onTriggered: pane.deleteCurrent() }
    }

    FileDialog {
        id: picker
        title: qsTr("Upload to %1").arg(browser.path)
        fileMode: FileDialog.OpenFiles
        onAccepted: browser.upload(selectedFiles)
    }

    PromptSheet { id: prompt }

    TSheet {
        id: confirm
        property var target: null
        width: Math.min(380, Overlay.overlay ? Overlay.overlay.width - 40 : 380)
        title: target ? qsTr("Delete %1?").arg(target.name) : ""
        contentItem: Text {
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            font.family: Style.fontFamily
            font.pixelSize: Style.fontSize
            color: Style.secondaryLabel
            text: confirm.target && confirm.target.folder
                  ? qsTr("Only empty folders can be deleted here.")
                  : qsTr("It's deleted from the server right away; there's no trash.")
        }
        footer: RowLayout {
            spacing: 10
            Item { implicitWidth: 10 }
            TButton { Layout.fillWidth: true; Layout.topMargin: 16; Layout.bottomMargin: 20; text: qsTr("Cancel"); onClicked: confirm.close() }
            TButton {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.bottomMargin: 20
                kind: "danger"
                text: qsTr("Delete")
                onClicked: {
                    browser.remove(confirm.target.name, confirm.target.folder)
                    confirm.close()
                }
            }
            Item { implicitWidth: 10 }
        }
    }
}
