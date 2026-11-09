import QtQuick
import QtTest
import Tether

// Settings: reachable by keyboard, readable on every accent, and careful
// with what can't be undone.
AppCase {
    name: "Settings"

    function init() { start() }
    function cleanup() { stop() }

    function section(index) {
        return inSheet(win.settingsSheet, "section" + index)
    }

    function test_arrowKeysMoveThroughTheSections() {
        if (touch)
            skip("no keyboard on a touch screen")
        win.settingsSheet.openAt(0)
        opened(win.settingsSheet)
        tryVerify(() => section(0).activeFocus, 5000, "the keyboard starts on the open section")
        keyClick(Qt.Key_Down)
        compare(win.settingsSheet.section, 1)
        verify(section(1).activeFocus)
        keyClick(Qt.Key_Down)
        compare(win.settingsSheet.section, 2)
        keyClick(Qt.Key_Up)
        keyClick(Qt.Key_Up)
        keyClick(Qt.Key_Up)
        compare(win.settingsSheet.section, 0)
        verify(section(0).activeFocus)
        // Tab leaves the list for the page beside it.
        keyClick(Qt.Key_Tab)
        verify(!section(0).activeFocus)
        compare(win.settingsSheet.section, 0)
    }

    function test_textOnEveryAccentReads() {
        win.settingsSheet.openAt(0)
        opened(win.settingsSheet)
        const name = find(section(0), "sectionName")
        for (const accent of Style.accents) {
            if (accent.key === "omarchy")
                continue
            compare(backend.setSetting("accent", accent.key), "")
            verify(same(Style.accent, accent.color), accent.key)
            // The text really takes the color meant for it (a property named
            // "on…" once never applied, and everything came out plain black).
            verify(same(name.color, Style.accentText), accent.key)
            verify(!same(name.color, "#000000"), accent.key)
            verify(contrastOn(Style.accentText, Style.accent) >= 4.5, accent.key + " " + contrastOn(Style.accentText, Style.accent))
            verify(contrastOn(Style.accentTextSecondary, Style.accent) >= 4.5, accent.key)
        }
        // A theme's dark accent gets light text.
        verify(same(Style.textOn("#1a1b26"), "white"))
        verify(contrastOn(Style.textOn("#3b4261"), Qt.color("#3b4261")) >= 4.5)
    }

    function test_restoringDefaultsAsksFirst() {
        compare(backend.setSetting("accent", "green"), "")
        win.settingsSheet.openAt(0)
        opened(win.settingsSheet)
        const restore = inSheet(win.settingsSheet, "restoreDefaults")
        verify(restore, "the Restore button is there")
        restore.clicked()
        opened(win.alert)
        compare(backend.settings.accent, "green")
        mouseClick(find(win.alert.contentItem.parent, "alertCancel"))
        closed(win.alert)
        compare(backend.settings.accent, "green")
        restore.clicked()
        opened(win.alert)
        mouseClick(find(win.alert.contentItem.parent, "alertConfirm"))
        tryCompare(backend.settings, "accent", "blue")
    }

    function test_accentSwatchesTakeArrowKeys() {
        if (touch)
            skip("no keyboard on a touch screen")
        win.settingsSheet.openAt(1)
        opened(win.settingsSheet)
        const blue = inSheet(win.settingsSheet, "accent-blue")
        verify(blue, "the swatches are there")
        blue.forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Right)
        verify(backend.settings.accent !== "blue")
        keyClick(Qt.Key_Left)
        compare(backend.settings.accent, "blue")
    }
}
