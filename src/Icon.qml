import QtQuick
import QtQuick.Layouts

// A line icon from icons/, drawn at size and dimmed with opacity. The files
// are white; color repaints one for a light background, such as the accent
// (through the "icon" image provider, see iconprovider.h).
Image {
    id: icon
    property string name
    property int size: 16
    property color color: "white"
    readonly property bool tinted: color.r < 0.99 || color.g < 0.99 || color.b < 0.99 || color.a < 0.99
    width: size
    height: size
    // In a layout too: it would otherwise go by the size the file is drawn at.
    Layout.preferredWidth: size
    Layout.preferredHeight: size
    sourceSize: Qt.size(size * 2, size * 2)
    source: !name ? ""
          : tinted ? "image://icon/" + name + "/" + [color.r, color.g, color.b, color.a].map(v => Math.round(v * 255)).join("-")
          : "qrc:/icons/" + name + ".svg"
    fillMode: Image.PreserveAspectFit
    // Drawn at twice the size and scaled down smoothly. No mipmaps: OpenGL ES
    // on Android can refuse them for sizes that aren't powers of two, and
    // then draws nothing.
    smooth: true
    // Decoration: whatever it sits in carries the name for screen readers.
    Accessible.ignored: true
}
