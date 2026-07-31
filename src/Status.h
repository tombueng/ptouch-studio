// Reading the printer status over the RFCOMM port — costs no tape.
//
// A P-touch replies to `ESC i S` with 32 status bytes; byte 10 holds the width of
// the inserted tape in millimetres. Documented in Brother's "Raster Command
// Reference" for the PT-P700 series.
#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QStringList>
#include <QThread>

namespace ptouch {

struct Status {
    bool ok = false;             // printer ready, tape inserted, no errors
    int tapeMm = 0;
    QString mediaType;
    QStringList errors;
    // Byte 18 is the kind of reply (0 = answer to a request, 1 = print finished,
    // 2 = error, 6 = phase change), byte 19 says whether the machine is printing.
    // Mixing the two up makes an idle printer look busy.
    int statusType = 0;
    int phase = 0;               // 0 = waiting for data, 1 = printing
    QString error;               // transport level failure (port, timeout)

    bool printing() const { return phase == 1; }
    bool reportsError() const { return statusType == 2 || !errors.isEmpty(); }
};

constexpr int StatusLength = 32;

// One attempt; opening the port is what establishes the Bluetooth connection.
Status queryStatus(const QString &device, int timeoutMs = 4000);

// Several attempts: after closing, the link needs a moment before it accepts again.
Status queryStatusRetry(const QString &device, int attempts = 3);

Status decodeStatus(const QByteArray &data);

// A character device that speaks the P-touch protocol.
struct Port {
    QString path;     // /dev/usb/lp0 or /dev/rfcomm0
    QString model;    // from the USB device ID, empty over Bluetooth
    bool usb = false;
};

// Ports worth trying, USB before Bluetooth. USB entries are filtered by their
// IEEE 1284 device ID, so other printers sharing /dev/usb/lp* are left alone.
QList<Port> candidatePorts();

// Sends the reset sequence; clears a pending error state without power-cycling.
bool resetPrinter(const QString &device, QString *error = nullptr);

// Queries in the background so the interface stays responsive.
class StatusWorker : public QThread {
    Q_OBJECT
public:
    explicit StatusWorker(QString device, QObject *parent = nullptr)
        : QThread(parent), m_device(std::move(device)) {}

signals:
    void finished(const ptouch::Status &status);

protected:
    void run() override;

private:
    QString m_device;
};

} // namespace ptouch
