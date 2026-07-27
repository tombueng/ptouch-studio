#include "Status.h"

#include <QByteArray>
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
        {0x00, QStringLiteral("no tape")},
        {0x01, QStringLiteral("laminated")},
        {0x03, QStringLiteral("non-laminated")},
        {0x04, QStringLiteral("fabric tape")},
        {0x11, QStringLiteral("heat-shrink tube")},
        {0x17, QStringLiteral("heat-shrink tube")},
        {0xFF, QStringLiteral("unknown")},
    };
    return m;
}

const QMap<int, QString> &errorBits1()
{
    static const QMap<int, QString> m = {
        {0x01, QStringLiteral("no tape inserted")},
        {0x02, QStringLiteral("end of tape")},
        {0x04, QStringLiteral("cutter jam")},
        {0x08, QStringLiteral("unsupported media")},
        {0x40, QStringLiteral("cover open")},
        {0x80, QStringLiteral("overheated")},
    };
    return m;
}

const QMap<int, QString> &errorBits2()
{
    static const QMap<int, QString> m = {
        {0x01, QStringLiteral("wrong tape width")},
        {0x04, QStringLiteral("buffer full")},
        {0x08, QStringLiteral("communication error")},
        {0x10, QStringLiteral("cover open")},
        {0x20, QStringLiteral("low voltage")},
        {0x40, QStringLiteral("printer busy")},
        {0x80, QStringLiteral("cannot print")},
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
    s.phase = u(18);
    s.ok = err1 == 0 && err2 == 0 && s.tapeMm > 0;
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

void StatusWorker::run()
{
    emit finished(queryStatusRetry(m_device));
}

} // namespace ptouch
