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
    smooth: true
    mipmap: true
}
