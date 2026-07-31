#include "Status.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QMap>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace ptouch {
namespace {

const QMap<int, QString> &mediaNames()
{
    static const QMap<int, QString> m = {
        {0x00, QCoreApplication::translate("Printer", "no tape")},
        {0x01, QCoreApplication::translate("Printer", "laminated")},
        {0x03, QCoreApplication::translate("Printer", "non-laminated")},
        {0x04, QCoreApplication::translate("Printer", "fabric tape")},
        {0x11, QCoreApplication::translate("Printer", "heat-shrink tube")},
        {0x17, QCoreApplication::translate("Printer", "heat-shrink tube")},
        {0xFF, QCoreApplication::translate("Printer", "unknown")},
    };
    return m;
}

const QMap<int, QString> &errorBits1()
{
    static const QMap<int, QString> m = {
        {0x01, QCoreApplication::translate("Printer", "no tape inserted")},
        {0x02, QCoreApplication::translate("Printer", "end of tape")},
        {0x04, QCoreApplication::translate("Printer", "cutter jam")},
        {0x08, QCoreApplication::translate("Printer", "unsupported media")},
        {0x40, QCoreApplication::translate("Printer", "cover open")},
        {0x80, QCoreApplication::translate("Printer", "overheated")},
    };
    return m;
}

const QMap<int, QString> &errorBits2()
{
    static const QMap<int, QString> m = {
        {0x01, QCoreApplication::translate("Printer", "wrong tape width")},
        {0x04, QCoreApplication::translate("Printer", "buffer full")},
        {0x08, QCoreApplication::translate("Printer", "communication error")},
        {0x10, QCoreApplication::translate("Printer", "cover open")},
        {0x20, QCoreApplication::translate("Printer", "low voltage")},
        {0x40, QCoreApplication::translate("Printer", "printer busy")},
        {0x80, QCoreApplication::translate("Printer", "cannot print")},
    };
    return m;
}

} // namespace

Status decodeStatus(const QByteArray &d)
{
    Status s;
    if (d.size() < StatusLength) {
        s.error = QStringLiteral("received only %1 of %2 status bytes")
                      .arg(d.size()).arg(StatusLength);
        return s;
    }
    const auto u = [&d](int i) { return static_cast<unsigned char>(d.at(i)); };
    const int err1 = u(8), err2 = u(9);

    s.tapeMm = u(10);
    s.mediaType = mediaNames().value(u(11),
                                     QStringLiteral("0x%1").arg(u(11), 2, 16, QLatin1Char('0')));
    for (auto it = errorBits1().begin(); it != errorBits1().end(); ++it)
        if (err1 & it.key())
            s.errors << it.value();
    for (auto it = errorBits2().begin(); it != errorBits2().end(); ++it)
        if (err2 & it.key())
            s.errors << it.value();
    s.statusType = u(18);
    s.phase = u(19);
    s.ok = err1 == 0 && err2 == 0 && s.tapeMm > 0 && s.statusType != 2;
    return s;
}

Status queryStatus(const QString &device, int timeoutMs)
{
    Status s;
    const QByteArray path = device.toLocal8Bit();

    // A blocking open is what brings up the Bluetooth connection.
    const int fd = ::open(path.constData(), O_RDWR | O_NOCTTY);
    if (fd < 0) {
        const int e = errno;
        if (e == EACCES)
            s.error = QStringLiteral("no access to %1 — setup incomplete").arg(device);
        else if (e == ENOENT)
            s.error = QStringLiteral("%1 is missing — is the RFCOMM service running?")
                          .arg(device);
        else
            s.error = QStringLiteral("%1: %2 — printer switched off or out of range?")
                          .arg(device, QString::fromLocal8Bit(std::strerror(e)));
        return s;
    }

    termios tio{};
    if (::tcgetattr(fd, &tio) == 0) {
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_iflag = tio.c_oflag = tio.c_lflag = 0;
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 10;
        ::tcsetattr(fd, TCSANOW, &tio);
    }

    // The printer sends status frames unprompted — on phase changes and on
    // errors. Those pile up, and reading the oldest one instead of the answer to
    // this request gives a stale picture: an idle machine can look faulty and a
    // blinking one ready. Discard whatever is waiting first.
    ::tcflush(fd, TCIFLUSH);
    char drain[StatusLength];
    while (true) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 50) <= 0)
            break;
        if (::read(fd, drain, sizeof(drain)) <= 0)
            break;
    }

    const QByteArray invalidate(100, '\0');
    const char init[] = {0x1b, 0x40};             // ESC @   initialise
    const char request[] = {0x1b, 0x69, 0x53};    // ESC i S status request
    const bool written = ::write(fd, invalidate.constData(), invalidate.size()) > 0
                         && ::write(fd, init, sizeof(init)) > 0
                         && ::write(fd, request, sizeof(request)) > 0;
    if (!written) {
        s.error = QStringLiteral("writing to %1 failed").arg(device);
        ::close(fd);
        return s;
    }

    QByteArray data;
    QElapsedTimer timer;
    timer.start();
    while (data.size() < StatusLength && timer.elapsed() < timeoutMs) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 250) > 0) {
            char buf[StatusLength];
            const ssize_t n = ::read(fd, buf, StatusLength - data.size());
            if (n > 0)
                data.append(buf, int(n));
        }
    }
    ::close(fd);

    if (data.size() < StatusLength) {
        s.error = data.isEmpty()
                      ? QStringLiteral("printer does not answer — switched on? "
                                       "(it powers off by itself after a while)")
                      : QStringLiteral("incomplete reply (%1 bytes)").arg(data.size());
        return s;
    }
    return decodeStatus(data);
}

Status queryStatusRetry(const QString &device, int attempts)
{
    Status s;
    for (int i = 0; i < attempts; ++i) {
        s = queryStatus(device);
        if (s.error.isEmpty())
            return s;
        if (s.error.contains(QStringLiteral("no access"))
            || s.error.contains(QStringLiteral("is missing")))
            return s;                       // setup problem: retrying will not help
        QThread::msleep(1000);
    }
    return s;
}

QString usbPrinterModel(const QString &node)
{
    // The IEEE 1284 device ID names manufacturer and model, e.g.
    // "MFG:Brother;MDL:PT-P710BT;...". Other printers share /dev/usb/lp*, and
    // writing status commands into the wrong one is not harmless — so ask first.
    QFile id(QStringLiteral("/sys/class/usbmisc/%1/device/ieee1284_id").arg(node));
    if (!id.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    const QString text = QString::fromLatin1(id.readAll());
    if (!text.contains(QStringLiteral("Brother"), Qt::CaseInsensitive))
        return {};

    static const QRegularExpression model(
        QStringLiteral("(?:MDL|MODEL):\\s*([^;]+)"), QRegularExpression::CaseInsensitiveOption);
    const auto m = model.match(text);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

bool resetPrinter(const QString &device, QString *error)
{
    // ESC @ clears the buffer and the error state. That is all a P-touch offers
    // as a remote reset — it cannot be power-cycled over the wire.
    const int fd = ::open(device.toLocal8Bit().constData(), O_RDWR | O_NOCTTY);
    if (fd < 0) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(device,
                         QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    const QByteArray invalidate(100, '\0');
    const char init[] = {0x1b, 0x40};
    const bool ok = ::write(fd, invalidate.constData(), invalidate.size()) > 0
                    && ::write(fd, init, sizeof(init)) > 0;
    ::tcdrain(fd);
    ::close(fd);
    if (!ok && error)
        *error = QStringLiteral("writing to %1 failed").arg(device);
    return ok;
}

QList<Port> candidatePorts()
{
    QList<Port> ports;

    // USB first: when the printer is plugged in that link needs no pairing and
    // does not fall asleep.
    const QDir usb(QStringLiteral("/dev/usb"));
    for (const QString &name : usb.entryList({QStringLiteral("lp*")}, QDir::System)) {
        const QString model = usbPrinterModel(name);
        if (!model.isEmpty())
            ports << Port{usb.filePath(name), model, true};
    }

    const QDir dev(QStringLiteral("/dev"));
    for (const QString &name : dev.entryList({QStringLiteral("rfcomm*")}, QDir::System))
        ports << Port{dev.filePath(name), QString(), false};

    return ports;
}

void StatusWorker::run()
{
    emit finished(queryStatusRetry(m_device));
}

} // namespace ptouch
