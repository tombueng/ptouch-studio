#include "Sdp.h"

#include <QElapsedTimer>
#include <QRegularExpression>
#include <QtEndian>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ptouch {
namespace {

// Kernel ABI from BlueZ' <bluetooth/bluetooth.h> and <bluetooth/l2cap.h>, spelled
// out so the build does not need libbluetooth-dev for two structs.
constexpr int AfBluetooth = 31;
constexpr int BtProtoL2cap = 0;
constexpr quint16 SdpPsm = 0x0001;

struct __attribute__((packed)) BdAddr {
    quint8 b[6];
};
struct SockAddrL2 {
    sa_family_t family;
    unsigned short psm;    // little endian
    BdAddr bdaddr;         // little endian: the address printed back to front
    unsigned short cid;
    quint8 bdaddrType;     // 0 = BR/EDR
};
static_assert(sizeof(BdAddr) == 6, "bdaddr_t is six bytes");

const QString &baseUuidSuffix()
{
    static const QString s = QStringLiteral("-0000-1000-8000-00805f9b34fb");
    return s;
}
QString uuid16(quint16 v)
{
    return QStringLiteral("%1").arg(v, 8, 16, QLatin1Char('0')) + baseUuidSuffix();
}
const QString &serialPortUuid()
{
    static const QString s = uuid16(0x1101);
    return s;
}
const QString &rfcommUuid()
{
    static const QString s = uuid16(0x0003);
    return s;
}
const QString &iap2Uuid()
{
    static const QString s = QStringLiteral("00000000-deca-fade-deca-deafdecacaff");
    return s;
}

// SDP data element (Bluetooth Core Specification, Vol 3, Part B, 3).
struct Element {
    enum Type { Nil = 0, UInt = 1, Int = 2, Uuid = 3, Text = 4, Bool = 5, Seq = 6, Alt = 7,
                Url = 8 };
    int type = -1;
    QByteArray value;           // scalar types
    QList<Element> items;       // sequences and alternatives

    bool isList() const { return type == Seq || type == Alt; }

    quint64 toUInt() const
    {
        quint64 v = 0;
        for (const char c : value)
            v = (v << 8) | quint8(c);
        return v;
    }

    QString toUuid() const
    {
        if (value.size() == 2 || value.size() == 4)
            return QStringLiteral("%1").arg(toUInt(), 8, 16, QLatin1Char('0')) + baseUuidSuffix();
        if (value.size() != 16)
            return {};
        const QString hex = QString::fromLatin1(value.toHex());
        return hex.mid(0, 8) + QLatin1Char('-') + hex.mid(8, 4) + QLatin1Char('-')
               + hex.mid(12, 4) + QLatin1Char('-') + hex.mid(16, 4) + QLatin1Char('-')
               + hex.mid(20, 12);
    }
};

bool parseElement(const uchar *&p, const uchar *end, Element &out, int depth)
{
    if (p >= end || depth > 8)
        return false;
    const uchar header = *p++;
    out.type = header >> 3;
    qint64 size = 0;
    switch (header & 0x07) {
    case 0: size = out.type == Element::Nil ? 0 : 1; break;
    case 1: size = 2; break;
    case 2: size = 4; break;
    case 3: size = 8; break;
    case 4: size = 16; break;
    case 5:
        if (end - p < 1)
            return false;
        size = *p;
        p += 1;
        break;
    case 6:
        if (end - p < 2)
            return false;
        size = qFromBigEndian<quint16>(p);
        p += 2;
        break;
    default:
        if (end - p < 4)
            return false;
        size = qFromBigEndian<quint32>(p);
        p += 4;
        break;
    }
    if (end - p < size)
        return false;

    if (out.isList()) {
        const uchar *inner = p;
        const uchar *innerEnd = p + size;
        while (inner < innerEnd) {
            Element child;
            if (!parseElement(inner, innerEnd, child, depth + 1))
                return false;
            out.items << child;
        }
    } else {
        out.value = QByteArray(reinterpret_cast<const char *>(p), int(size));
    }
    p += size;
    return true;
}

// ProtocolDescriptorList: { { L2CAP }, { RFCOMM, channel } }, possibly offered as
// several alternatives.
int rfcommChannel(const Element &protocols)
{
    if (protocols.type == Element::Alt) {
        for (const Element &alternative : protocols.items)
            if (const int channel = rfcommChannel(alternative))
                return channel;
        return 0;
    }
    for (const Element &layer : protocols.items) {
        if (!layer.isList() || layer.items.size() < 2)
            continue;
        const Element &id = layer.items.at(0);
        const Element &parameter = layer.items.at(1);
        if (id.type == Element::Uuid && id.toUuid() == rfcommUuid()
            && parameter.type == Element::UInt) {
            const quint64 channel = parameter.toUInt();
            if (channel >= 1 && channel <= quint64(MaxRfcommChannel))
                return int(channel);
        }
    }
    return 0;
}

SdpService serviceFromRecord(const Element &record)
{
    SdpService s;
    for (int i = 0; i + 1 < record.items.size(); i += 2) {
        const Element &id = record.items.at(i);
        const Element &value = record.items.at(i + 1);
        if (id.type != Element::UInt)
            continue;
        switch (id.toUInt()) {
        case 0x0001:    // ServiceClassIDList
            for (const Element &c : value.items)
                if (c.type == Element::Uuid)
                    s.classes << c.toUuid();
            break;
        case 0x0004:    // ProtocolDescriptorList
            s.channel = rfcommChannel(value);
            break;
        case 0x0100:    // ServiceName (primary language)
            if (value.type == Element::Text) {
                QByteArray text = value.value;
                const int nul = text.indexOf('\0');
                if (nul >= 0)
                    text.truncate(nul);
                s.name = QString::fromUtf8(text).trimmed();
            }
            break;
        default:
            break;
        }
    }
    return s;
}

QString shortUuid(const QString &uuid)
{
    if (uuid == serialPortUuid())
        return QStringLiteral("Serial Port");
    if (uuid == iap2Uuid())
        return QStringLiteral("Apple iAP2");
    if (uuid.endsWith(baseUuidSuffix()) && uuid.startsWith(QStringLiteral("0000")))
        return QStringLiteral("0x") + uuid.mid(4, 4);
    return uuid;
}

void appendBigEndian16(QByteArray &out, quint16 v)
{
    out += char(v >> 8);
    out += char(v & 0xff);
}

bool parseMac(const QString &mac, BdAddr &out)
{
    static const QRegularExpression re(
        QStringLiteral("^([0-9A-Fa-f]{2}):([0-9A-Fa-f]{2}):([0-9A-Fa-f]{2}):"
                       "([0-9A-Fa-f]{2}):([0-9A-Fa-f]{2}):([0-9A-Fa-f]{2})$"));
    const auto m = re.match(mac.trimmed());
    if (!m.hasMatch())
        return false;
    for (int i = 0; i < 6; ++i)
        out.b[5 - i] = quint8(m.captured(i + 1).toUInt(nullptr, 16));
    return true;
}

bool sameService(const SdpService &a, const SdpService &b)
{
    return a.channel == b.channel && a.name == b.name && a.classes == b.classes;
}

struct SocketGuard {
    int fd;
    ~SocketGuard()
    {
        if (fd >= 0)
            ::close(fd);
    }
};

} // namespace

bool SdpService::isSerialPort() const
{
    return classes.contains(serialPortUuid());
}

bool SdpService::isIap2() const
{
    return classes.contains(iap2Uuid()) || name.contains(QStringLiteral("iap"), Qt::CaseInsensitive);
}

QString SdpService::describe() const
{
    QStringList classNames;
    for (const QString &c : classes)
        classNames << shortUuid(c);
    return QStringLiteral("%1  \"%2\"  [%3]")
        .arg(channel > 0 ? QStringLiteral("channel %1").arg(channel, 2)
                         : QStringLiteral("no RFCOMM"),
             name, classNames.join(QStringLiteral(", ")));
}

QList<SdpService> parseSdpAttributeLists(const QByteArray &attributeLists)
{
    QList<SdpService> services;
    const auto *p = reinterpret_cast<const uchar *>(attributeLists.constData());
    const uchar *end = p + attributeLists.size();
    Element top;
    if (!parseElement(p, end, top, 0) || !top.isList())
        return services;
    for (const Element &record : std::as_const(top.items))
        if (record.isList())
            services << serviceFromRecord(record);
    return services;
}

int pickSerialChannel(const QList<SdpService> &services)
{
    for (const SdpService &s : services)
        if (s.channel > 0 && s.isSerialPort() && !s.isIap2())
            return s.channel;

    // No record says "Serial Port". A single remaining RFCOMM service is still a
    // safe bet; with several, guessing would be no better than channel 1.
    QList<int> channels;
    for (const SdpService &s : services)
        if (s.channel > 0 && !s.isIap2() && !channels.contains(s.channel))
            channels << s.channel;
    return channels.size() == 1 ? channels.first() : 0;
}

QList<SdpService> queryRfcommServices(const QString &mac, QString *error, int timeoutMs)
{
    QList<SdpService> services;
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return services;
    };

    SockAddrL2 addr{};
    addr.family = AfBluetooth;
    addr.psm = qToLittleEndian(SdpPsm);
    if (!parseMac(mac, addr.bdaddr))
        return fail(QStringLiteral("\"%1\" is not a Bluetooth address").arg(mac));

    const int fd = ::socket(AfBluetooth, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
                            BtProtoL2cap);
    if (fd < 0)
        return fail(QStringLiteral("no Bluetooth socket: %1")
                        .arg(QString::fromLocal8Bit(std::strerror(errno))));
    const SocketGuard guard{fd};

    QElapsedTimer timer;
    timer.start();
    const auto remaining = [&] {
        return int(std::max<qint64>(0, timeoutMs - timer.elapsed()));
    };

    if (::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0) {
        if (errno != EINPROGRESS && errno != EAGAIN)
            return fail(QStringLiteral("cannot reach %1: %2")
                            .arg(mac, QString::fromLocal8Bit(std::strerror(errno))));
        pollfd pfd{fd, POLLOUT, 0};
        const int ready = ::poll(&pfd, 1, remaining());
        if (ready == 0)
            return fail(QStringLiteral("no answer from %1 — switched on and in range?").arg(mac));
        int soError = 0;
        socklen_t length = sizeof(soError);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &length);
        if (ready < 0 || soError != 0)
            return fail(QStringLiteral("cannot reach %1: %2")
                            .arg(mac, QString::fromLocal8Bit(
                                          std::strerror(soError ? soError : errno))));
    }

    // Browse first, like `sdptool browse`, so the log shows everything the device
    // offers. Only if that turns up no serial port, ask for that class directly —
    // not every device files its services under the public browse group.
    quint16 transaction = 0;
    QString lastError;
    for (const quint16 pattern : {quint16(0x1002), quint16(0x1101)}) {
        QByteArray lists;
        QByteArray continuation;
        bool complete = false;
        for (int part = 0; part < 32 && !complete; ++part) {
            QByteArray params;
            params += QByteArray::fromHex("350319");          // { UUID16 pattern }
            appendBigEndian16(params, pattern);
            appendBigEndian16(params, 0xffff);                // maximum byte count
            params += QByteArray::fromHex("3509090001090004090100");  // 0x0001, 0x0004, 0x0100
            params += char(continuation.size());
            params += continuation;

            QByteArray pdu;
            pdu += char(0x06);                                // ServiceSearchAttributeRequest
            appendBigEndian16(pdu, ++transaction);
            appendBigEndian16(pdu, quint16(params.size()));
            pdu += params;
            if (::send(fd, pdu.constData(), size_t(pdu.size()), MSG_NOSIGNAL) != pdu.size()) {
                lastError = QStringLiteral("sending the SDP request failed: %1")
                                .arg(QString::fromLocal8Bit(std::strerror(errno)));
                break;
            }

            pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, remaining()) <= 0) {
                lastError = QStringLiteral("%1 did not answer the SDP request").arg(mac);
                break;
            }
            QByteArray reply(0x10000, '\0');
            const ssize_t n = ::recv(fd, reply.data(), size_t(reply.size()), MSG_DONTWAIT);
            if (n < 5) {
                lastError = QStringLiteral("short SDP reply");
                break;
            }
            reply.truncate(int(n));
            const auto *r = reinterpret_cast<const uchar *>(reply.constData());
            const quint16 parameterLength = qFromBigEndian<quint16>(r + 3);
            if (qFromBigEndian<quint16>(r + 1) != transaction || n < 5 + parameterLength) {
                lastError = QStringLiteral("malformed SDP reply");
                break;
            }
            if (r[0] == 0x01) {                               // ErrorResponse
                lastError = QStringLiteral("SDP error 0x%1")
                                .arg(parameterLength >= 2 ? qFromBigEndian<quint16>(r + 5) : 0,
                                     4, 16, QLatin1Char('0'));
                break;
            }
            if (r[0] != 0x07 || parameterLength < 3) {
                lastError = QStringLiteral("unexpected SDP reply 0x%1").arg(r[0], 2, 16,
                                                                             QLatin1Char('0'));
                break;
            }
            const quint16 count = qFromBigEndian<quint16>(r + 5);
            if (parameterLength < 2 + count + 1) {
                lastError = QStringLiteral("malformed SDP reply");
                break;
            }
            lists += reply.mid(7, count);
            const int continuationLength = r[7 + count];
            if (continuationLength > 16 || parameterLength < 2 + count + 1 + continuationLength) {
                lastError = QStringLiteral("malformed SDP continuation");
                break;
            }
            continuation = reply.mid(8 + count, continuationLength);
            complete = continuationLength == 0;
        }
        if (!complete)
            continue;

        for (const SdpService &s : parseSdpAttributeLists(lists)) {
            const bool known = std::any_of(services.cbegin(), services.cend(),
                                           [&s](const SdpService &k) { return sameService(k, s); });
            if (!known)
                services << s;
        }
        if (std::any_of(services.cbegin(), services.cend(),
                        [](const SdpService &s) { return s.channel > 0 && s.isSerialPort(); }))
            break;
    }

    if (services.isEmpty())
        return fail(lastError.isEmpty() ? QStringLiteral("%1 announces no services").arg(mac)
                                        : lastError);
    return services;
}

bool looksLikeIap2(const QByteArray &reply)
{
    // A P-touch status frame starts with 0x80; iAP2 link packets with FF 55.
    return !reply.isEmpty() && quint8(reply.at(0)) != 0x80
           && reply.contains(QByteArray::fromHex("ff55"));
}

} // namespace ptouch
