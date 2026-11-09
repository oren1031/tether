import QtQuick
import QtTest
import Tether

// The narrow window of a phone held upright.
AppCase {
    name: "Phone"

    function init() { startPhone() }
    function cleanup() { stop() }

    function test_theListFillsANarrowWindow() {
        verify(win.narrow)
        compare(win.hostsSidebar.width, win.width)
    }

    // Back, in a session, goes to the list of hosts instead of asking to quit.
    function test_backFromASessionShowsTheHosts() {
        if (!touch)
            skip("the Back key is a touch-screen thing")
        mouseClick(hostRow(0))
        tryCompare(win, "tabCount", 1)
        tryCompare(win, "sidebarVisible", false)
        win.close()
        verify(win.visible, "Tether is still open")
        verify(win.sidebarVisible, "the hosts are showing")
        verify(!win.alert.visible, "nothing asks about quitting")
        compare(win.tabCount, 1)
    }

    function test_whatAFingerTapsIsBigEnough() {
        if (!touch)
            skip("pointer sizes are checked by eye")
        verify(hostRow(0).height >= 44)
        win.hostSheet.openFor({ kind: "ssh" })
        opened(win.hostSheet)
        verify(inSheet(win.hostSheet, "saveHost").height >= 44)
        verify(inSheet(win.hostSheet, "keepRunning").height >= 44)
        verify(inSheet(win.hostSheet, "nameField").height >= 44)
    }
}
