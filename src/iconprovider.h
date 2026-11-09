#pragma once

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QQuickImageProvider>
#include <QRegularExpression>
#include <QSvgRenderer>

// Tether's icons are white line drawings. This hands QML one repainted in
// another color, for an icon on a light fill such as the accent:
// image://icon/<name>/<red>-<green>-<blue>-<alpha>, each 0 to 255. Painted
// here rather than by a shader effect, so it looks the same under software
// rendering and on every phone's graphics driver.
class IconProvider : public QQuickImageProvider {
public:
    IconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override {
        static const QRegularExpression valid("^([a-z0-9-]+)/(\\d{1,3})-(\\d{1,3})-(\\d{1,3})-(\\d{1,3})$");
        const auto match = valid.match(id);
        QFile file(":/icons/" + match.captured(1) + ".svg");
        if (!match.hasMatch() || !file.open(QIODevice::ReadOnly))
            return {};
        const QColor color(qMin(match.captured(2).toInt(), 255), qMin(match.captured(3).toInt(), 255),
                           qMin(match.captured(4).toInt(), 255));
        QByteArray svg = file.readAll();
        svg.replace("#ffffff", color.name().toLatin1());
        QSvgRenderer renderer(svg);
        const QSize pixels = requestedSize.isEmpty() ? QSize(48, 48) : requestedSize;
        QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setOpacity(qMin(match.captured(5).toInt(), 255) / 255.0);
        renderer.render(&painter);
        if (size)
            *size = pixels;
        return image;
    }
};
