import QtQuick
import QtTest
import Tether

// The list of hosts and the form that adds one.
AppCase {
    name: "Hosts"

    function init() { start() }
    function cleanup() { stop() }

    function openForm() {
        mouseClick(find(win.contentItem, "newHost"))
        opened(win.hostSheet)
    }

    function test_newHostButtonOpensTheForm() {
        openForm()
        const name = inSheet(win.hostSheet, "nameField")
        verify(name.activeFocus, "typing starts in Name")
    }

    // Rows of the form start on one line whatever is hidden (the "keep
    // running" note used to push later rows a cell sideways).
    function test_formRowsLineUp() {
        openForm()
        const keep = inSheet(win.hostSheet, "keepRunning"), again = inSheet(win.hostSheet, "reconnect")
        verify(keep.visible && again.visible)
        compare(again.mapToItem(null, 0, 0).x, keep.mapToItem(null, 0, 0).x)
        keep.checked = true
        drawn()
        compare(again.mapToItem(null, 0, 0).x, keep.mapToItem(null, 0, 0).x)
    }

    function test_savingWithoutANameSaysSoAndFocusesIt() {
        openForm()
        inSheet(win.hostSheet, "hostField").forceActiveFocus()
        mouseClick(inSheet(win.hostSheet, "saveHost"))
        const error = inSheet(win.hostSheet, "hostError")
        verify(win.hostSheet.visible, "the form stays open")
        verify(error.text !== "", "an error is shown")
        // Beside the buttons, inside the sheet: never scrolled out of sight.
        const save = inSheet(win.hostSheet, "saveHost")
        const at = error.mapToItem(null, 0, 0), saveAt = save.mapToItem(null, 0, 0)
        verify(Math.abs((at.y + error.height / 2) - (saveAt.y + save.height / 2)) < save.height,
               "the error sits in the row of buttons")
        verify(inSheet(win.hostSheet, "nameField").activeFocus, "the cursor is in the empty Name")
        compare(backend.profiles.length, 0)
    }

    function test_savingAHostListsIt() {
        openForm()
        type("web-01")
        keyClick(Qt.Key_Tab)
        verify(inSheet(win.hostSheet, "hostField").activeFocus, "Tab goes from Name to Host")
        type("10.0.0.5")
        mouseClick(inSheet(win.hostSheet, "saveHost"))
        closed(win.hostSheet)
        compare(backend.profiles.length, 1)
        compare(backend.profiles[0].name, "web-01")
        compare(backend.profiles[0].host, "10.0.0.5")
        compare(find(hostRow(1), "hostName").text, "web-01")
    }

    function test_aClickBesideTheFormKeepsIt() {
        openForm()
        type("half-filled")
        mouseClick(win.contentItem, 8, 8)
        wait(300)
        verify(win.hostSheet.visible, "a stray click doesn't lose the form")
        compare(inSheet(win.hostSheet, "nameField").text, "half-filled")
        keyClick(Qt.Key_Escape)
        closed(win.hostSheet)
    }

    // A finger taps once to connect; a mouse selects, and connects on a
    // double click.
    function test_tapOrDoubleClickConnects() {
        const row = hostRow(0)
        mouseClick(row)
        if (touch) {
            tryCompare(win, "tabCount", 1)
            return
        }
        wait(100)
        compare(win.tabCount, 0)
        mouseDoubleClickSequence(row)
        tryCompare(win, "tabCount", 1)
    }

    function test_holdingARowOpensItsMenuOnTouch() {
        const row = hostRow(0)
        mousePress(row)
        wait(1200)
        const open = win.hostsSidebar.menuOpen
        mouseRelease(row)
        compare(open, touch)
        // Holding isn't a tap: nothing connected.
        compare(win.tabCount, 0)
    }

    function test_closingATabWithALiveSessionAsks() {
        win.openTab({ kind: "local" })
        tryCompare(win, "tabCount", 1)
        tryVerify(() => win.currentPage && win.currentPage.liveCount() === 1)
        win.closeTab(0)
        opened(win.alert)
        compare(win.tabCount, 1)
        mouseClick(find(win.alert.contentItem.parent, "alertCancel"))
        closed(win.alert)
        compare(win.tabCount, 1)
        win.closeTab(0)
        opened(win.alert)
        mouseClick(find(win.alert.contentItem.parent, "alertConfirm"))
        tryCompare(win, "tabCount", 0)
    }

    function test_selectedRowTextReadsOnTheAccent() {
        const row = hostRow(0)
        const list = find(win.contentItem, "hostList")
        list.forceActiveFocus()
        tryVerify(() => row.strong)
        const name = find(row, "hostName")
        verify(same(name.color, Style.accentText))
        verify(contrastOn(name.color, Style.accent) >= 4.5)
    }
}
