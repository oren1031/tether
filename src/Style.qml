pragma Singleton
import QtQuick

// Tether's look, after macOS in dark mode: layered gray surfaces, translucent
// white for text and fills, one accent color, and soft rounded corners. The
// accent, the fonts, and the terminal's colors come from Settings.
QtObject {
    id: style

    readonly property var settings: backend.settings

    // The accent colors Settings offers, in the order it shows them.
    readonly property var accents: [
        { key: "blue", name: qsTr("Blue"), color: "#0A84FF" },
        { key: "purple", name: qsTr("Purple"), color: "#BF5AF2" },
        { key: "pink", name: qsTr("Pink"), color: "#FF375F" },
        { key: "red", name: qsTr("Red"), color: "#FF453A" },
        { key: "orange", name: qsTr("Orange"), color: "#FF9F0A" },
        { key: "yellow", name: qsTr("Yellow"), color: "#FFD60A" },
        { key: "green", name: qsTr("Green"), color: "#30D158" },
        { key: "graphite", name: qsTr("Graphite"), color: "#8E8E93" },
        { key: "omarchy", name: qsTr("Omarchy theme"), color: "" }
    ]

    function accentFor(key) {
        if (key === "omarchy")
            return theme.accent
        for (const a of accents)
            if (a.key === key)
                return a.color
        return accents[0].color
    }

    readonly property string fontFamily: {
        if (settings.interfaceFont)
            return settings.interfaceFont
        const families = Qt.fontFamilies()
        for (const f of ["Inter", "Inter Variable", "SF Pro Text", "Noto Sans", "Cantarell"])
            if (families.includes(f))
                return f
        return Qt.application.font.family
    }
    readonly property string monoFamily: "monospace"
    readonly property string terminalFamily: settings.terminalFont || "monospace"

    // Surfaces, from the back of the window to the front.
    readonly property color window: "#1E1E20"
    readonly property color sidebar: "#262628"
    readonly property color toolbar: "#2A2A2C"
    readonly property color sheet: "#2C2C2E"
    readonly property color popover: "#323234"
    readonly property color separator: Qt.rgba(0, 0, 0, 0.55)
    readonly property color hairline: Qt.rgba(1, 1, 1, 0.08)

    // Text.
    readonly property color label: Qt.rgba(1, 1, 1, 0.88)
    // Each step still reads at 4.5:1 or better on every surface above,
    // fields and grouped rows included (tertiary is 4.6:1 at its worst).
    readonly property color secondaryLabel: Qt.rgba(1, 1, 1, 0.72)
    readonly property color tertiaryLabel: Qt.rgba(1, 1, 1, 0.60)
    // Errors as text; Style.red is for fills and is too dark to read.
    readonly property color errorText: "#FF7A70"

    // Controls.
    readonly property color fill: Qt.rgba(1, 1, 1, 0.09)
    readonly property color fillHover: Qt.rgba(1, 1, 1, 0.13)
    readonly property color fillPressed: Qt.rgba(1, 1, 1, 0.18)
    readonly property color fieldFill: Qt.rgba(1, 1, 1, 0.06)
    // The edge of a field, checkbox, or switch: 3:1 against what's around it.
    readonly property color fieldBorder: Qt.rgba(1, 1, 1, 0.45)
    readonly property color selection: Qt.rgba(1, 1, 1, 0.10)

    readonly property color accent: accentFor(settings.accent)
    readonly property color accentHover: Qt.lighter(accent, 1.12)
    readonly property color accentPressed: Qt.darker(accent, 1.15)
    // The ring around whatever has keyboard focus: the accent, or near-white
    // when a theme's accent is too dark to show on these surfaces.
    readonly property color focusRing: contrast(accent, sheet) >= 3 ? accent : Qt.rgba(1, 1, 1, 0.85)
    // Text and icons on the accent, whichever of near-black and white reads
    // better: near-black on all eight accents here (5.1:1 or more, where
    // white is 1.4 to 3.7), white on a theme's dark accent. Never name these
    // "on…": QML takes such a name for a signal handler and drops the value.
    readonly property color accentText: textOn(accent)
    readonly property color accentTextSecondary: secondaryTextOn(accent)

    function tint(alpha) {
        return Qt.rgba(accent.r, accent.g, accent.b, alpha)
    }

    // WCAG relative luminance and contrast ratio of opaque colors.
    function luminance(c) {
        const f = v => v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4)
        return 0.2126 * f(c.r) + 0.7152 * f(c.g) + 0.0722 * f(c.b)
    }
    function contrast(a, b) {
        const la = luminance(a), lb = luminance(b)
        return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05)
    }
    function darkTextOn(fill) {
        const dark = Qt.rgba(fill.r * 0.15, fill.g * 0.15, fill.b * 0.15, 1)
        return contrast(dark, fill) >= contrast(Qt.rgba(1, 1, 1, 1), fill)
    }
    // Text and icons for a filled shape of any color.
    function textOn(fill) {
        return darkTextOn(fill) ? Qt.rgba(0, 0, 0, 0.85) : Qt.rgba(1, 1, 1, 1)
    }
    function secondaryTextOn(fill) {
        return darkTextOn(fill) ? Qt.rgba(0, 0, 0, 0.8) : Qt.rgba(1, 1, 1, 0.85)
    }

    readonly property color blue: "#0A84FF"
    readonly property color red: "#FF453A"
    readonly property color orange: "#FF9F0A"
    readonly property color green: "#30D158"
    readonly property color purple: "#BF5AF2"
    readonly property color indigo: "#5E5CE6"
    readonly property color gray: "#8E8E93"

    readonly property int radiusSmall: 6
    readonly property int radius: 8
    readonly property int radiusLarge: 12

    // Touch screens get bigger text and targets; compact is set by the window
    // when it's phone-narrow, and sheets then fill it.
    readonly property bool touch: backend.touchUi
    property bool compact: false
    // How far the system's bars, a camera cutout, and the on-screen keyboard
    // reach into the window; set by the window. The main window stays inside
    // them on its own, sheets and menus are placed by these.
    property real safeTop: 0
    property real safeBottom: 0
    property real safeLeft: 0
    property real safeRight: 0
    readonly property int fontSize: (settings.interfaceFontSize || 13) + (touch ? 2 : 0)
    readonly property int smallFontSize: fontSize - 2
    readonly property int titleFontSize: fontSize + 2
    readonly property int controlHeight: touch ? 44 : fontSize + 15
    readonly property int toolbarButton: touch ? 44 : 28
    // Rows in menus, pop-up lists, and file lists; a finger needs 44.
    readonly property int rowHeight: touch ? 44 : 26
    readonly property bool mirrored: Qt.application.layoutDirection === Qt.RightToLeft

    readonly property int fast: 120
    readonly property int normal: 200

    // The terminal's colors: the Omarchy theme's, or one of Tether's schemes.
    readonly property var terminalColors: {
        theme.revision
        const colors = theme.schemeColors(settings.terminalScheme)
        return colors.background ? colors : theme.schemeColors("omarchy")
    }
    readonly property color terminalBackground: terminalColors.background
    readonly property color terminalForeground: terminalColors.foreground || "#e6e6e6"
}
