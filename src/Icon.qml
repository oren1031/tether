import QtQuick

// A white line icon from icons/, drawn at size and dimmed with opacity.
Image {
    property string name
    property int size: 16
    width: size
    height: size
    sourceSize: Qt.size(size * 2, size * 2)
    source: name ? "qrc:/icons/" + name + ".svg" : ""
    fillMode: Image.PreserveAspectFit
    // Drawn at twice the size and scaled down smoothly. No mipmaps: OpenGL ES
    // on Android can refuse them for sizes that aren't powers of two, and
    // then draws nothing.
    smooth: true
}
