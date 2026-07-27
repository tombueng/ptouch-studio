// Which printer, which queue, which RFCOMM port.
// Nothing is hard-wired: environment > user configuration > system configuration.
#pragma once

#include <QString>

namespace ptouch {

struct Config {
    QString printer;                                  // CUPS queue
    QString device = QStringLiteral("/dev/rfcomm0");  // port used for status queries
    QString mac;                                      // Bluetooth address
    QString model;                                    // e.g. PT-P710BT

    static Config load();
    void save() const;

    static QString configFilePath();
};

// First CUPS queue that uses the ptouch driver.
QString findPtouchQueue();

// Values from /etc/ptouch-studio/rfcomm.conf, written by the system setup.
QString systemConfValue(const QString &key);

constexpr const char *SystemConfPath = "/etc/ptouch-studio/rfcomm.conf";

} // namespace ptouch
