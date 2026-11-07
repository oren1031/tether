#pragma once

#include <QObject>
#include <QTimer>
#include <QVariant>

// Serial devices that are plugged in right now (USB adapters, boards with a
// USB CDC port, Bluetooth serial), refreshed while the app runs.
class SerialPorts : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)

public:
    explicit SerialPorts(const QString &deviceDirectory = "/dev", int interval = 1500, QObject *parent = nullptr);

    QVariantList devices() const { return m_devices; }
    Q_INVOKABLE void refresh();

    // Why the device can't be opened, in words the user can act on; empty
    // when it can.
    static QString problem(const QString &path);
    // Whether Tether can open serial ports itself (Qt Serial Port), for
    // systems without picocom.
    static bool builtIn();
    // "usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0" -> "FTDI FT232R USB UART A50285BI".
    static QString labelFor(const QString &byIdName);

signals:
    void devicesChanged();

private:
    QString m_directory;
    QVariantList m_devices;
    QTimer m_timer;
};
