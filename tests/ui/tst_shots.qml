import QtQuick
import QtTest
import Tether

// Not a test of anything: with TETHER_SHOTS=<directory> this saves pictures
// of the main screens, to look at after a change to the interface. Add
// TETHER_TOUCH=1 for the phone, TETHER_UI_LANGUAGE=he for right-to-left.
AppCase {
    name: "Shots"

    readonly property string mode: (touch ? "phone" : "desktop") + (harness.language === "en" ? "" : "-" + harness.language)

    function shot(name) {
        wait(350)
        drawn()
        verify(harness.grab(win, mode + "-" + name), name)
    }

    function seed() {
        backend.saveProfile({ kind: "ssh", name: "web-01", host: "10.0.0.5", user: "deploy", port: 22, reconnect: true })
        backend.saveProfile({ kind: "ssh", name: "שרת הבית", host: "home.example.com", user: "oren", port: 2222, reconnect: true })
        backend.saveProfile({ kind: "serial", name: "ESP32 console", device: "/dev/ttyUSB0", baud: 115200, dataBits: 8,
                              parity: "none", stopBits: 1, flow: "none", enter: "cr" })
    }

    function test_shots() {
        if (!harness.shots)
            skip("set TETHER_SHOTS to a directory to save screenshots")
        seed()
        if (touch)
            startPhone()
        else
            start()
        shot("main")
        find(win.contentItem, "hostList").forceActiveFocus()
        shot("main-list-focused")

        win.hostSheet.openFor({ kind: "ssh" })
        opened(win.hostSheet)
        shot("newhost")
        mouseClick(inSheet(win.hostSheet, "saveHost"))
        shot("newhost-error")
        inSheet(win.hostSheet, "keepRunning").checked = true
        shot("newhost-keep-running")
        win.hostSheet.close()
        closed(win.hostSheet)
        win.hostSheet.openFor({ kind: "serial" })
        opened(win.hostSheet)
        shot("newserial")
        win.hostSheet.close()
        closed(win.hostSheet)

        const sections = win.settingsSheet.sections.length
        for (let i = 0; i < sections; ++i) {
            if (touch && win.settingsSheet.sections[i].desktop)
                continue
            win.settingsSheet.openAt(i)
            opened(win.settingsSheet)
            if (touch) {
                win.settingsSheet.section = i
                win.settingsSheet.showingPage = true
            }
            shot("settings-" + i)
            win.settingsSheet.close()
            closed(win.settingsSheet)
        }
        if (touch) {
            win.settingsSheet.openAt(0)
            opened(win.settingsSheet)
            win.settingsSheet.showingPage = false
            shot("settings-list")
            win.settingsSheet.close()
            closed(win.settingsSheet)
        }

        backend.setSetting("accent", "yellow")
        find(win.contentItem, "hostList").forceActiveFocus()
        shot("yellow-main")
        win.settingsSheet.openAt(1)
        opened(win.settingsSheet)
        shot("yellow-settings")
        win.settingsSheet.close()
        closed(win.settingsSheet)
        win.hostSheet.openFor({ kind: "ssh" })
        opened(win.hostSheet)
        shot("yellow-newhost")
        win.hostSheet.close()
        closed(win.hostSheet)
        backend.setSetting("accent", "blue")

        // A session, the pane menu's button, and the Ask sheet on a light scheme.
        backend.setSetting("terminalScheme", "Tether Light")
        win.openTab({ kind: "local" })
        tryCompare(win, "tabCount", 1)
        shot("session-light")
        win.currentPage.split(Qt.Horizontal)
        const panes = find(win.contentItem, "paneButton")
        mouseClick(panes)
        shot("pane-menu")
        keyClick(Qt.Key_Escape)
        win.askSheet.openFor(win.currentPage)
        opened(win.askSheet)
        assistant.answered("du -ah /var | sort -rh | head -n 10", "Lists the ten biggest files and folders under /var.", true)
        shot("ask-light")
        win.askSheet.close()
        closed(win.askSheet)
        win.closeTab(0)
        opened(win.alert)
        shot("close-tab-question")
        mouseClick(find(win.alert.contentItem.parent, "alertConfirm"))
        tryCompare(win, "tabCount", 0)
        stop()
    }
}
