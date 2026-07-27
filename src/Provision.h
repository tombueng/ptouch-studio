// System setup: pairing, RFCOMM port, CUPS backend and print queue.
//
// Every step can be checked and carried out on its own, so the wizard in the
// interface walks exactly the same path as the command line setup.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace ptouch {

struct Device {
    QString mac;
    QString name;
    QString model;
};

struct SetupState {
    bool haveBluetoothctl = false;
    bool haveRfcomm = false;
    bool haveLp = false;
    bool haveDriver = false;      // ptouch printer driver installed
    bool paired = false;
    bool udevRule = false;
    bool systemdUnit = false;
    bool rfcommActive = false;
    bool devicePresent = false;
    bool deviceAccessible = false;
    bool cupsBackend = false;
    bool queuePresent = false;

    QString device;
    QString queue;
    QString mac;
    QString model;

    bool dependenciesOk() const {
        return haveBluetoothctl && haveRfcomm && haveLp && haveDriver;
    }
    bool ready() const {
        return dependenciesOk() && paired && rfcommActive && deviceAccessible
               && cupsBackend && queuePresent;
    }
    QStringList missingSteps() const;
};

SetupState checkSetup();

// Looks for P-touch devices in range (takes `seconds`).
QList<Device> scanForPrinters(int seconds = 12);

// Pairs and trusts the device. An existing phone connection does not get in the way.
bool pairDevice(const QString &mac, QString *output = nullptr);

QString modelFromName(const QString &name);

// CUPS backend directory of this distribution.
QString cupsBackendDir();

// Everything that needs root — idempotent. Runs under pkexec or sudo.
int installSystem(const QString &mac, const QString &model, const QString &queue,
                  int index, int channel, const QString &owner,
                  const std::function<void(const QString &)> &log);

// Command line the interface uses to trigger the system part through pkexec.
QStringList systemSetupCommand(const QString &mac, const QString &model,
                               const QString &queue, const QString &owner);

constexpr const char *DefaultQueue = "PT-Label";

} // namespace ptouch
