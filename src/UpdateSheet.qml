import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Tether

// A newer Tether: what changed, and a button that downloads and installs it.
TSheet {
    id: sheet
    signal installRequested(var command)

    large: true
    width: preferredWidth(520)
    height: preferredHeight(520)
    title: updater.available ? qsTr("Tether %1 is available").arg(updater.latestVersion) : qsTr("Updates")
    subtitle: qsTr("You have %1.").arg(updater.currentVersion)

    contentItem: ColumnLayout {
        spacing: 12

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 10
            color: Qt.rgba(0, 0, 0, 0.18)
            border.color: Qt.rgba(1, 1, 1, 0.06)
            ScrollView {
                id: notesView
                anchors.fill: parent
                anchors.margins: 14
                clip: true
                contentWidth: availableWidth
                Text {
                    width: notesView.availableWidth
                    wrapMode: Text.Wrap
                    textFormat: Text.MarkdownText
                    font.family: Style.fontFamily
                    font.pixelSize: Style.fontSize
                    color: Style.label
                    linkColor: Style.accent
                    text: updater.notes || qsTr("No release notes.")
                    // The notes come from the network: only web links open.
                    onLinkActivated: link => { if (String(link).startsWith("https://")) Qt.openUrlExternally(link) }
                }
            }
        }

        // Download progress.
        Rectangle {
            visible: updater.state === "downloading"
            Layout.fillWidth: true
            implicitHeight: 6
            radius: 3
            color: Style.fill
            Rectangle {
                width: parent.width * updater.progress
                height: parent.height
                radius: 3
                color: Style.accent
                Behavior on width { NumberAnimation { duration: 150 } }
            }
        }

        Text {
            visible: updater.message !== ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.family: Style.fontFamily
            font.pixelSize: Style.smallFontSize + 1
            color: updater.state === "error" ? Style.errorText : Style.secondaryLabel
            text: updater.message
                  + (updater.state === "ready" && updater.installKind === "pacman"
                     ? " " + qsTr("Installing opens a tab where sudo asks for your password; restart Tether when it's done.")
                     : updater.state === "ready" && updater.installKind === "android"
                     ? " " + qsTr("Android asks to install it over this version; allow installing from Tether if it asks.") : "")
        }
    }

    footer: RowLayout {
        spacing: 10
        TButton {
            Layout.leftMargin: 20
            Layout.bottomMargin: 20
            kind: "plain"
            leftPadding: 0
            text: qsTr("Release page")
            visible: updater.releaseUrl.startsWith("https://")
            onClicked: Qt.openUrlExternally(updater.releaseUrl)
        }
        Item { Layout.fillWidth: true }
        TButton { Layout.bottomMargin: 20; text: qsTr("Later"); onClicked: sheet.close() }
        TButton {
            Layout.bottomMargin: 20
            Layout.rightMargin: 20
            kind: "primary"
            enabled: updater.available && updater.state !== "downloading" && updater.state !== "checking"
            text: updater.state === "ready" ? qsTr("Install")
                : updater.installKind === "page" ? qsTr("Get it")
                : qsTr("Download")
            onClicked: {
                if (updater.installKind === "page") {
                    if (updater.releaseUrl.startsWith("https://"))
                        Qt.openUrlExternally(updater.releaseUrl)
                    sheet.close()
                } else if (updater.state === "ready") {
                    sheet.installRequested(updater.installCommand())
                    sheet.close()
                } else {
                    updater.download()
                }
            }
        }
    }
}
