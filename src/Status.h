// Reading the printer status over the RFCOMM port — costs no tape.
//
// A P-touch replies to `ESC i S` with 32 status bytes; byte 10 holds the width of
// the inserted tape in millimetres. Documented in Brother's "Raster Command
// Reference" for the PT-P700 series.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

namespace ptouch {

struct Status {
    bool ok = false;             // printer ready, tape inserted, no errors
    int tapeMm = 0;
    QString mediaType;
    QStringList errors;
    int phase = 0;               // 0 = waiting for data, 1 = printing
    QString error;               // transport level failure (port, timeout)

    bool printing() const { return phase == 1; }
};

constexpr int StatusLength = 32;

// One attempt; opening the port is what establishes the Bluetooth connection.
Status queryStatus(const QString &device, int timeoutMs = 4000);

// Several attempts: after closing, the link needs a moment before it accepts again.
Status queryStatusRetry(const QString &device, int attempts = 3);

Status decodeStatus(const QByteArray &data);

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
