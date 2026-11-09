import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Tether

// What the sync folder wants to change that Tether won't take on its word:
// a host with options that run programs, or a change to where a saved host
// connects or what is typed into it. A pill says something is waiting; the
// sheet shows each item, before and after, to accept or turn down.
Item {
    id: review

    readonly property var items: backend.syncReview
    // Options that run programs are accepted one at a time, each looked at.
    readonly property bool anyRisky: items.some(item => item.risky.length > 0)

    Rectangle {
        id: pill
        visible: review.items.length > 0 && !sheet.visible
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: Style.toolbarButton + 20
        width: Math.min(parent.width - 24, pillRow.implicitWidth + 24)
        height: Style.controlHeight + 12
        radius: height / 2
        color: Style.popover
        border.color: Style.orange

        RowLayout {
            id: pillRow
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 6
            spacing: 10
            Text {
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.family: Style.fontFamily
                font.pixelSize: Style.fontSize
                color: Style.label
                text: qsTr("Sync wants to change %n items", "", review.items.length)
            }
            TButton {
                text: qsTr("Review")
                onClicked: sheet.open()
            }
        }
    }

    TSheet {
        id: sheet
        parent: Overlay.overlay
        large: true
        property string error: ""
        width: preferredWidth(560)
        title: qsTr("Changes from sync")
        closePolicy: Popup.CloseOnEscape

        function decide(accept, id) {
            error = accept ? backend.approveSync(id) : backend.rejectSync(id)
            if (!error && review.items.length === 0)
                close()
        }

        contentItem: ScrollView {
            id: scroll
            implicitHeight: Math.min(list.implicitHeight, sheet.preferredHeight(680) - 150)
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                id: list
                width: scroll.availableWidth
                spacing: 12

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.family: Style.fontFamily
                    font.pixelSize: Style.fontSize
                    color: Style.secondaryLabel
                    text: qsTr("These came through the sync folder, from another of your computers or from anything else that can write there. They change where a host connects or what is typed into it, so nothing here is used until you accept it.")
                }

                Repeater {
                    model: review.items
                    delegate: Rectangle {
                        id: card
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: cardColumn.implicitHeight + 24
                        radius: Style.radius
                        color: Style.fill
                        border.color: modelData.risky.length ? Style.orange : Style.hairline

                        ColumnLayout {
                            id: cardColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 12
                            spacing: 6

                            Text {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                font.family: Style.fontFamily
                                font.pixelSize: Style.fontSize
                                font.weight: Font.DemiBold
                                color: Style.label
                                text: card.modelData.name
                            }
                            Text {
                                Layout.fillWidth: true
                                font.family: Style.fontFamily
                                font.pixelSize: Style.smallFontSize
                                color: Style.secondaryLabel
                                text: card.modelData.type === "host" ? (card.modelData.isNew ? qsTr("New host") : qsTr("Changed host"))
                                    : card.modelData.type === "snippet" ? (card.modelData.isNew ? qsTr("New snippet") : qsTr("Changed snippet"))
                                    : (card.modelData.isNew ? qsTr("New login") : qsTr("Changed login"))
                            }
                            Text {
                                visible: card.modelData.risky.length > 0
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                font.family: Style.fontFamily
                                font.pixelSize: Style.fontSize
                                color: Style.orange
                                text: qsTr("Its ssh options run a program on this computer or turn off checks of the server: %1")
                                          .arg(card.modelData.risky.join(", "))
                            }
                            Repeater {
                                model: card.modelData.changes
                                delegate: ColumnLayout {
                                    id: change
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        font.family: Style.fontFamily
                                        font.pixelSize: Style.smallFontSize
                                        color: Style.secondaryLabel
                                        text: change.modelData.label
                                    }
                                    Text {
                                        visible: !card.modelData.isNew || change.modelData.before !== ""
                                        Layout.fillWidth: true
                                        wrapMode: Text.WrapAnywhere
                                        textFormat: Text.PlainText
                                        font.family: Style.monoFamily
                                        font.pixelSize: Style.smallFontSize + 1
                                        font.strikeout: true
                                        color: Style.secondaryLabel
                                        text: change.modelData.before !== "" ? change.modelData.before : qsTr("(empty)")
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WrapAnywhere
                                        textFormat: Text.PlainText
                                        font.family: Style.monoFamily
                                        font.pixelSize: Style.smallFontSize + 1
                                        color: Style.label
                                        text: change.modelData.after !== "" ? change.modelData.after : qsTr("(empty)")
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                spacing: 8
                                Item { Layout.fillWidth: true }
                                TButton {
                                    text: card.modelData.isNew ? qsTr("Don't add") : qsTr("Keep mine")
                                    onClicked: sheet.decide(false, card.modelData.id)
                                }
                                TButton {
                                    kind: card.modelData.risky.length ? "danger" : "default"
                                    text: qsTr("Accept")
                                    onClicked: sheet.decide(true, card.modelData.id)
                                }
                            }
                        }
                    }
                }

                Text {
                    visible: sheet.error !== ""
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.family: Style.fontFamily
                    font.pixelSize: Style.fontSize
                    color: Style.red
                    text: sheet.error
                }
            }
        }

        footer: RowLayout {
            spacing: 10
            TButton {
                Layout.leftMargin: 20
                Layout.bottomMargin: 20
                visible: !review.anyRisky
                text: qsTr("Accept all")
                onClicked: sheet.decide(true, "")
            }
            Item { Layout.fillWidth: true }
            TButton { Layout.bottomMargin: 20; text: qsTr("Turn all down"); onClicked: sheet.decide(false, "") }
            TButton {
                Layout.bottomMargin: 20
                Layout.rightMargin: 20
                kind: "primary"
                text: qsTr("Later")
                onClicked: sheet.close()
            }
        }
    }
}
