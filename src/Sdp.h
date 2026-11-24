// Which RFCOMM channel the printer's serial port listens on.
//
// The channel is not the same on every machine. A PT-P710BT with newer firmware
// puts Apple's iAP2 service on channel 1 and the serial port on channel 2; binding
// channel 1 there gets a stream of iAP2 handshake packets (FF 55 02 00 EE 10)
// instead of status replies, and print jobs vanish without a trace. The printer
// itself says where each service lives, through SDP — the Bluetooth service
// directory. This asks it directly over an L2CAP socket, so neither libbluetooth
// nor the deprecated sdptool is needed.
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace ptouch {

struct SdpService {
    int channel = 0;          // RFCOMM channel, 0 if the record has none
    QString name;             // ServiceName, e.g. "SPP SERVER"
    QStringList classes;      // ServiceClassIDList as 128-bit UUID strings

    bool isSerialPort() const;
    bool isIap2() const;      // Apple accessory protocol — never the printer
    QString describe() const; // one line for logs and the diagnostics report
};

// Decodes the AttributeLists of an SDP ServiceSearchAttributeResponse (all
// continuation parts joined). Records without an RFCOMM channel are kept, with
// channel 0, so the diagnostics show everything the device announced.
QList<SdpService> parseSdpAttributeLists(const QByteArray &attributeLists);

// The channel to bind for printing: a Serial Port record first, iAP2 never.
// 0 when the list gives no usable answer.
int pickSerialChannel(const QList<SdpService> &services);

// Asks the device at `mac` for its RFCOMM services. Works without root and
// without the device being connected, but it has to be switched on.
QList<SdpService> queryRfcommServices(const QString &mac, QString *error = nullptr,
                                      int timeoutMs = 10000);

// Bytes read from the port that come from the iAP2 service rather than from the
// printer — the telltale of a port bound to the wrong channel.
bool looksLikeIap2(const QByteArray &reply);

// RFCOMM channels are 1–30.
constexpr int MaxRfcommChannel = 30;

} // namespace ptouch
