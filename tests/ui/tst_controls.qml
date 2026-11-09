import QtQuick
import QtTest
import Tether

// Tether's own controls, one at a time: keys, holding, and what a screen
// reader is told.
Item {
    id: stage
    width: 400
    height: 300

TestCase {
    id: test
    name: "Controls"
    when: windowShown

    function make(file, properties) {
        const component = Qt.createComponent("qrc:/" + file)
        verify(component.status === Component.Ready, component.errorString())
        const item = createTemporaryObject(component, stage, properties || {})
        verify(item)
        return item
    }

    SignalSpy { id: spy }

    function test_stepperTakesKeysAndRepeatsWhileHeld() {
        const stepper = make("TStepper.qml", { value: 10, from: 7, to: 40, label: "Text size" })
        spy.target = stepper
        spy.signalName = "modified"
        spy.clear()
        stepper.modified.connect(v => stepper.value = v)
        stepper.forceActiveFocus()
        keyClick(Qt.Key_Up)
        compare(stepper.value, 11)
        keyClick(Qt.Key_Down)
        keyClick(Qt.Key_Down)
        compare(stepper.value, 9)
        compare(stepper.Accessible.name, "Text size")
        // Held on +, it keeps going: far more than the one step of a click.
        const before = stepper.value
        mousePress(stepper, stepper.width - 8, stepper.height / 2)
        wait(1100)
        mouseRelease(stepper, stepper.width - 8, stepper.height / 2)
        verify(stepper.value >= before + 4, "held: " + before + " → " + stepper.value)
        const after = stepper.value
        wait(300)
        compare(stepper.value, after)
        // And stops at the end of its range.
        stepper.value = 39
        keyClick(Qt.Key_Up)
        keyClick(Qt.Key_Up)
        compare(stepper.value, 40)
    }

    function test_segmentedTakesArrowKeys() {
        const control = make("TSegmented.qml", { model: [{ value: "a", text: "One" }, { value: "b", text: "Two" }, { value: "c", text: "Three" }],
                                                 value: "a", label: "Pick" })
        control.forceActiveFocus()
        keyClick(Qt.Key_Right)
        compare(control.value, "b")
        keyClick(Qt.Key_Right)
        keyClick(Qt.Key_Right)
        compare(control.value, "c")
        keyClick(Qt.Key_Left)
        compare(control.value, "b")
        mouseClick(control, 10, control.height / 2)
        compare(control.value, "a")
        compare(control.Accessible.name, "Pick")
    }

    function test_iconButtonsHaveNames() {
        const button = make("TButton.qml", { kind: "toolbar", iconName: "gear", label: "Settings" })
        compare(button.Accessible.name, "Settings")
        const worded = make("TButton.qml", { text: "Save" })
        compare(worded.Accessible.name, "Save")
    }

    function test_switchedOffControlsLookIt() {
        for (const file of ["TField.qml", "TSwitch.qml", "TCheck.qml", "TCombo.qml", "TTextArea.qml", "TSegmented.qml", "TStepper.qml"]) {
            const control = make(file, { enabled: false })
            verify(control.opacity < 0.6, file)
            control.enabled = true
            compare(control.opacity, 1, file)
        }
    }

    function test_fieldsAreNamedForScreenReaders() {
        const field = make("TField.qml", { placeholderText: "web-01", label: "Name" })
        compare(field.Accessible.name, "Name")
        const bare = make("TField.qml", { placeholderText: "Search" })
        compare(bare.Accessible.name, "Search")
        const check = make("TCheck.qml", { text: "Keep it running" })
        compare(check.Accessible.name, "Keep it running")
    }
}
}
