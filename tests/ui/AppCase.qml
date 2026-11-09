import QtQuick
import QtTest
import Tether

// A test case with Tether's whole window to drive. start() opens a fresh
// window, stop() closes it and removes what the test saved.
TestCase {
    id: test
    when: windowShown

    property var win: null
    readonly property bool touch: backend.touchUi

    function start(properties) {
        const component = Qt.createComponent("qrc:/Main.qml")
        verify(component.status === Component.Ready, component.errorString())
        win = component.createObject(null, properties || {})
        verify(win, "the window opens")
        tryVerify(() => win.visible)
        win.requestActivate()
        tryVerify(() => win.active)
        drawn()
    }

    function stop() {
        if (win) {
            win.quitting = true
            win.destroy()
            win = null
        }
        for (const profile of backend.profiles)
            backend.removeProfile(profile.id)
        backend.resetSettings()
    }

    // Waits for the window to be drawn as it is now. Asked for, since a
    // window with nothing moving in it draws no further frames to wait on.
    function drawn() {
        win.update()
        waitForRendering(win.contentItem, 1000)
    }

    // A phone held upright.
    function startPhone() {
        start({ minimumWidth: 0, minimumHeight: 0, width: 390, height: 844 })
    }

    // The first item named name under root, through every level.
    function find(root, name) {
        if (!root)
            return null
        if (root.objectName === name)
            return root
        const children = root.children || []
        for (let i = 0; i < children.length; ++i) {
            const hit = find(children[i], name)
            if (hit)
                return hit
        }
        return null
    }

    // Looks inside a sheet: its title, its content, and its buttons.
    function inSheet(sheet, name) {
        return find(sheet.contentItem.parent, name)
    }

    function hostRow(index) {
        const list = find(win.contentItem, "hostList")
        verify(list, "the host list is there")
        tryVerify(() => list.itemAtIndex(index) !== null)
        return list.itemAtIndex(index)
    }

    function type(text) {
        for (const letter of text)
            keyClick(letter)
    }

    function opened(sheet) {
        tryVerify(() => sheet.opened, 5000, "the sheet opens")
    }

    function closed(sheet) {
        tryVerify(() => !sheet.visible, 5000, "the sheet closes")
    }

    // Whether two colors are the same, see-through part included.
    function same(a, b) {
        return String(Qt.color(a)) === String(Qt.color(b))
    }

    // Contrast of a color that may be see-through, drawn over fill.
    function contrastOn(color, fill) {
        const flat = Qt.rgba(color.r * color.a + fill.r * (1 - color.a), color.g * color.a + fill.g * (1 - color.a),
                             color.b * color.a + fill.b * (1 - color.a), 1)
        return Style.contrast(flat, fill)
    }
}
