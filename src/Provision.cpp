#include "Provision.h"

#include "Config.h"
#include "Status.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <QTextStream>
#include <QThread>

#include <unistd.h>

namespace ptouch {
namespace {

constexpr const char *UdevRulePath = "/etc/udev/rules.d/70-ptouch-rfcomm.rules";
constexpr const char *UsbUdevRulePath = "/etc/udev/rules.d/70-ptouch-usb.rules";
constexpr const char *SystemdUnitPath = "/etc/systemd/system/ptouch-rfcomm.service";

// Bluetooth names start with the model designation, usually with a serial number
// appended (e.g. "PT-P710BT3015").
const QRegularExpression &modelPattern()
{
    static const QRegularExpression re(QStringLiteral("\\b(PT-[A-Z0-9]+|QL-[0-9A-Z]+)\\b"));
    return re;
}

QString run(const QString &program, const QStringList &args,
            int *exitCode = nullptr, int timeoutMs = 20000)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        if (exitCode)
            *exitCode = -1;
        return QString::fromLocal8Bit(p.readAll());
    }
    if (exitCode)
        *exitCode = p.exitCode();
    return QString::fromLocal8Bit(p.readAll());
}

bool writeFile(const QString &path, const QString &content, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(content.toUtf8());
    return true;
}

QString udevRule(const QString &owner)
{
    return QStringLiteral(
        "# P-touch Studio: make RFCOMM ports reachable for the CUPS backend (group lp)\n"
        "# and for the logged-in desktop user. Group membership alone is not enough —\n"
        "# it only takes effect after the next login.\n"
        "KERNEL==\"rfcomm[0-9]*\", GROUP=\"lp\", MODE=\"0660\", TAG+=\"uaccess\"%1\n")
        .arg(owner.isEmpty() ? QString() : QStringLiteral(", OWNER=\"%1\"").arg(owner));
}

QString usbUdevRule(const QString &owner)
{
    return QStringLiteral(
        "# P-touch Studio: Brother label printers on USB (vendor 04f9). The kernel\n"
        "# hands these to group lp only, but reading the loaded tape happens from\n"
        "# the desktop session.\n"
        "SUBSYSTEM==\"usbmisc\", KERNEL==\"lp[0-9]*\", ATTRS{idVendor}==\"04f9\", "
        "GROUP=\"lp\", MODE=\"0660\", TAG+=\"uaccess\"%1\n")
        .arg(owner.isEmpty() ? QString() : QStringLiteral(", OWNER=\"%1\"").arg(owner));
}

QString systemdUnit()
{
    return QStringLiteral(
        "[Unit]\n"
        "Description=RFCOMM port for Brother P-touch label printer\n"
        "Documentation=https://github.com/tombueng/ptouch-studio\n"
        "After=bluetooth.service\n"
        "Requires=bluetooth.service\n"
        "\n"
        "[Service]\n"
        "Type=oneshot\n"
        "RemainAfterExit=yes\n"
        "EnvironmentFile=%1\n"
        "ExecStart=/usr/bin/rfcomm bind ${PTOUCH_RFCOMM_INDEX} ${PTOUCH_MAC} ${PTOUCH_CHANNEL}\n"
        "ExecStop=/usr/bin/rfcomm release ${PTOUCH_RFCOMM_INDEX}\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n")
        .arg(QString::fromLatin1(SystemConfPath));
}

// Find the driver that matches a device name.
//
// The Bluetooth name carries the serial number ("PT-P710BT3015"), so a direct
// comparison fails. The driver list decides: we look for the longest model in it
// that the device name starts with. Without that check you end up with the first
// P-touch driver in the list — and its tape widths belong to another machine.
struct DriverMatch {
    QString ppd;
    QString model;
};

DriverMatch findDriver(const QString &deviceName)
{
    const QString out = run(QStringLiteral("lpinfo"), {QStringLiteral("-m")});
    const QStringList lines = out.split(QLatin1Char('\n'));
    const QString needle = QString(deviceName).remove(QLatin1Char(' ')).toUpper();

    DriverMatch best;
    QString fallback;
    for (const QString &line : lines) {
        if (!line.contains(QStringLiteral("ptouch"), Qt::CaseInsensitive))
            continue;
        if (fallback.isEmpty() && line.contains(QStringLiteral("ptouch-pt"), Qt::CaseInsensitive))
            fallback = line.section(QLatin1Char(' '), 0, 0);

        const auto m = modelPattern().match(line);
        if (!m.hasMatch())
            continue;
        const QString model = m.captured(1).toUpper();
        if (needle.startsWith(model) && model.size() > best.model.size())
            best = {line.section(QLatin1Char(' '), 0, 0), model};
    }
    if (best.ppd.isEmpty())
        best.ppd = fallback;         // some P-touch driver beats no driver at all
    return best;
}

// Device name from the Bluetooth database, in case no model was passed in.
QString deviceNameFor(const QString &mac)
{
    const QString info = run(QStringLiteral("bluetoothctl"), {QStringLiteral("info"), mac});
    for (const QString &line : info.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QStringLiteral("Name:")))
            return trimmed.mid(5).trimmed();
    }
    return {};
}

} // namespace

QStringList SetupState::missingSteps() const
{
    QStringList todo;
    QStringList missing;
    if (!haveLp)     missing << QStringLiteral("cups-client");
    if (!haveDriver) missing << QStringLiteral("printer-driver-ptouch");
    if (link == Link::Bluetooth) {
        if (!haveBluetoothctl) missing << QStringLiteral("bluez");
        if (!haveRfcomm)       missing << QStringLiteral("bluez (rfcomm)");
    }
    if (!missing.isEmpty())
        todo << QStringLiteral("install packages: %1").arg(missing.join(QStringLiteral(", ")));

    if (link == Link::Bluetooth) {
        if (!paired)
            todo << QStringLiteral("pair the printer");
        if (!(udevRule && systemdUnit && rfcommActive))
            todo << QStringLiteral("set up the RFCOMM port");
        else if (!deviceAccessible)
            todo << QStringLiteral("fix the port permissions");
        if (!cupsBackend)
            todo << QStringLiteral("install the CUPS backend");
    } else if (!deviceAccessible) {
        todo << QStringLiteral("fix the permissions on %1").arg(device);
    }

    if (!queuePresent)
        todo << QStringLiteral("create the print queue");
    return todo;
}

QString modelFromName(const QString &name)
{
    const auto m = modelPattern().match(name);
    return m.hasMatch() ? m.captured(1) : QString();
}

QString cupsBackendDir()
{
    for (const QString &dir : {QStringLiteral("/usr/lib/cups/backend"),
                               QStringLiteral("/usr/libexec/cups/backend")}) {
        if (QFileInfo::exists(dir))
            return dir;
    }
    return QStringLiteral("/usr/lib/cups/backend");
}

SetupState checkSetup()
{
    const Config cfg = Config::load();
    SetupState s;
    s.device = cfg.device;
    s.queue = cfg.printer;
    s.mac = cfg.mac;
    s.model = cfg.model;
    // The configured device decides which half of the setup has to be in place.
    s.link = cfg.device.startsWith(QStringLiteral("/dev/usb/")) ? Link::Usb
                                                                : Link::Bluetooth;

    const auto have = [](const QString &tool) {
        return !QStandardPaths::findExecutable(tool).isEmpty();
    };
    s.haveBluetoothctl = have(QStringLiteral("bluetoothctl"));
    s.haveRfcomm = have(QStringLiteral("rfcomm"));
    s.haveLp = have(QStringLiteral("lp"));
    s.haveDriver = s.haveLp && run(QStringLiteral("lpinfo"), {QStringLiteral("-m")})
                                   .contains(QStringLiteral("ptouch"), Qt::CaseInsensitive);

    if (!cfg.mac.isEmpty()) {
        const QString info = run(QStringLiteral("bluetoothctl"),
                                 {QStringLiteral("info"), cfg.mac});
        s.paired = info.contains(QStringLiteral("Paired: yes"));
        if (s.model.isEmpty())
            s.model = modelFromName(info);
    }

    s.udevRule = QFileInfo::exists(QString::fromLatin1(UdevRulePath));
    s.systemdUnit = QFileInfo::exists(QString::fromLatin1(SystemdUnitPath));
    s.rfcommActive = run(QStringLiteral("systemctl"),
                         {QStringLiteral("is-active"), QStringLiteral("ptouch-rfcomm.service")})
                         .trimmed() == QStringLiteral("active");
    s.devicePresent = QFileInfo::exists(cfg.device);
    s.deviceAccessible = s.devicePresent
                         && ::access(cfg.device.toLocal8Bit().constData(), R_OK | W_OK) == 0;
    s.cupsBackend = QFileInfo::exists(cupsBackendDir() + QStringLiteral("/rfcomm"));

    if (!cfg.printer.isEmpty()) {
        int code = 0;
        run(QStringLiteral("lpstat"), {QStringLiteral("-p"), cfg.printer}, &code);
        s.queuePresent = code == 0;
    }
    return s;
}

QList<Device> scanForPrinters(int seconds)
{
    QProcess scan;
    scan.start(QStringLiteral("bluetoothctl"),
               {QStringLiteral("--timeout"), QString::number(seconds), QStringLiteral("scan"),
                QStringLiteral("on")});
    scan.waitForFinished((seconds + 10) * 1000);
    const QString scanOutput = QString::fromLocal8Bit(scan.readAll());
    const QString known = run(QStringLiteral("bluetoothctl"), {QStringLiteral("devices")});

    QMap<QString, QString> found;
    static const QRegularExpression line(
        QStringLiteral("([0-9A-Fa-f:]{17})\\s+([^\\r\\n]+)"));
    for (const QString &source : {known, scanOutput}) {
        auto it = line.globalMatch(source);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString name = m.captured(2).trimmed();
            if (modelPattern().match(name).hasMatch())
                found.insert(m.captured(1).toUpper(), name);
        }
    }

    QList<Device> devices;
    for (auto it = found.begin(); it != found.end(); ++it)
        devices << Device{it.key(), it.value(), modelFromName(it.value())};
    return devices;
}

bool pairDevice(const QString &mac, QString *output)
{
    // Scan briefly so the adapter knows about the device.
    QProcess scan;
    scan.start(QStringLiteral("bluetoothctl"),
               {QStringLiteral("--timeout"), QStringLiteral("6"), QStringLiteral("scan"),
                QStringLiteral("on")});
    scan.waitForFinished(20000);

    QString log;
    log += run(QStringLiteral("bluetoothctl"), {QStringLiteral("pair"), mac}, nullptr, 45000);
    log += run(QStringLiteral("bluetoothctl"), {QStringLiteral("trust"), mac}, nullptr, 20000);
    const QString info = run(QStringLiteral("bluetoothctl"), {QStringLiteral("info"), mac});
    if (output)
        *output = log.trimmed();
    return info.contains(QStringLiteral("Paired: yes"));
}

QList<UsbPrinter> scanUsbPrinters()
{
    QList<UsbPrinter> printers;

    // CUPS knows the device URIs; the kernel nodes carry the IEEE 1284 ID.
    const QString uris = run(QStringLiteral("lpinfo"), {QStringLiteral("-v")});
    QStringList brotherUris;
    for (const QString &line : uris.split(QLatin1Char('\n'))) {
        if (line.contains(QStringLiteral("usb://"), Qt::CaseInsensitive)
            && line.contains(QStringLiteral("brother"), Qt::CaseInsensitive))
            brotherUris << line.section(QLatin1Char(' '), 1).trimmed();
    }

    const QList<Port> ports = candidatePorts();
    for (const QString &uri : std::as_const(brotherUris)) {
        UsbPrinter printer;
        printer.uri = uri;
        static const QRegularExpression modelInUri(QStringLiteral("usb://[^/]+/([^?]+)"));
        const auto m = modelInUri.match(uri);
        if (m.hasMatch())
            printer.model = QUrl::fromPercentEncoding(m.captured(1).toUtf8());

        // Match the kernel node by model name; falls back to the first USB port.
        for (const Port &port : ports) {
            if (!port.usb)
                continue;
            if (printer.devicePath.isEmpty() || port.model == printer.model)
                printer.devicePath = port.path;
        }
        printers << printer;
    }
    return printers;
}

int installSystemUsb(const QString &uri, const QString &model, const QString &queue,
                     const QString &owner,
                     const std::function<void(const QString &)> &log)
{
    if (::geteuid() != 0) {
        log(QStringLiteral("This step needs administrator rights."));
        return 1;
    }
    QString error;

    log(QStringLiteral("udev rule %1").arg(QString::fromLatin1(UsbUdevRulePath)));
    if (!writeFile(QString::fromLatin1(UsbUdevRulePath), usbUdevRule(owner), &error)) {
        log(QStringLiteral("  failed: %1").arg(error));
        return 1;
    }
    run(QStringLiteral("udevadm"), {QStringLiteral("control"), QStringLiteral("--reload")});
    run(QStringLiteral("udevadm"), {QStringLiteral("trigger"),
                                    QStringLiteral("--subsystem-match=usbmisc")});

    const DriverMatch driver = findDriver(model);
    if (driver.ppd.isEmpty()) {
        log(QStringLiteral("No P-touch driver found — please install printer-driver-ptouch "
                           "(or ptouch-driver)."));
        return 2;
    }

    log(QStringLiteral("queue %1 using %2").arg(queue, driver.ppd));
    int code = 0;
    const QString lpadmin = run(
        QStringLiteral("lpadmin"),
        {QStringLiteral("-p"), queue, QStringLiteral("-E"),
         QStringLiteral("-v"), uri,
         QStringLiteral("-m"), driver.ppd,
         QStringLiteral("-D"), QStringLiteral("Brother %1 (USB)")
                                   .arg(model.isEmpty() ? QStringLiteral("P-touch") : model),
         QStringLiteral("-L"), QStringLiteral("USB")}, &code);
    if (code != 0) {
        log(QStringLiteral("  lpadmin: %1").arg(lpadmin.trimmed()));
        return 3;
    }
    run(QStringLiteral("cupsenable"), {queue});
    run(QStringLiteral("cupsaccept"), {queue});

    log(QStringLiteral("Done — no RFCOMM service or custom backend needed over USB."));
    return 0;
}

QStringList systemSetupCommandUsb(const QString &uri, const QString &model,
                                  const QString &queue, const QString &owner)
{
    return {QCoreApplication::applicationFilePath(),
            QStringLiteral("setup-system"),
            QStringLiteral("--usb"), uri,
            QStringLiteral("--model"), model,
            QStringLiteral("--queue"), queue,
            QStringLiteral("--owner"), owner};
}

QStringList systemSetupCommand(const QString &mac, const QString &model,
                               const QString &queue, const QString &owner)
{
    return {QCoreApplication::applicationFilePath(),
            QStringLiteral("setup-system"),
            QStringLiteral("--mac"), mac,
            QStringLiteral("--model"), model,
            QStringLiteral("--queue"), queue,
            QStringLiteral("--owner"), owner};
}

int installSystem(const QString &mac, const QString &model, const QString &queue,
                  int index, int channel, const QString &owner,
                  const std::function<void(const QString &)> &log)
{
    if (::geteuid() != 0) {
        log(QStringLiteral("This step needs administrator rights."));
        return 1;
    }
    const QString device = QStringLiteral("/dev/rfcomm%1").arg(index);
    QString error;

    log(QStringLiteral("configuration %1").arg(QString::fromLatin1(SystemConfPath)));
    const QString conf = QStringLiteral("# written by ptouch-studio setup\n"
                                        "PTOUCH_MAC=%1\n"
                                        "PTOUCH_RFCOMM_INDEX=%2\n"
                                        "PTOUCH_CHANNEL=%3\n"
                                        "PTOUCH_DEVICE=%4\n")
                             .arg(mac).arg(index).arg(channel).arg(device);
    if (!writeFile(QString::fromLatin1(SystemConfPath), conf, &error)) {
        log(QStringLiteral("  failed: %1").arg(error));
        return 1;
    }

    log(QStringLiteral("udev rule %1").arg(QString::fromLatin1(UdevRulePath)));
    if (!writeFile(QString::fromLatin1(UdevRulePath), udevRule(owner), &error)) {
        log(QStringLiteral("  failed: %1").arg(error));
        return 1;
    }
    run(QStringLiteral("udevadm"), {QStringLiteral("control"), QStringLiteral("--reload")});

    log(QStringLiteral("systemd service %1").arg(QString::fromLatin1(SystemdUnitPath)));
    if (!writeFile(QString::fromLatin1(SystemdUnitPath), systemdUnit(), &error)) {
        log(QStringLiteral("  failed: %1").arg(error));
        return 1;
    }
    run(QStringLiteral("systemctl"), {QStringLiteral("daemon-reload")});
    run(QStringLiteral("rfcomm"), {QStringLiteral("release"), QString::number(index)});
    run(QStringLiteral("systemctl"), {QStringLiteral("enable"),
                                      QStringLiteral("ptouch-rfcomm.service")});
    // Deliberately restart rather than "enable --now": with RemainAfterExit the
    // service already counts as active, so "--now" would do nothing at all and the
    // port would stay unbound.
    int code = 0;
    const QString unitOut = run(QStringLiteral("systemctl"),
                                {QStringLiteral("restart"),
                                 QStringLiteral("ptouch-rfcomm.service")}, &code);
    if (code != 0)
        log(QStringLiteral("  service did not start: %1").arg(unitOut.trimmed()));

    QThread::msleep(1500);
    if (QFileInfo::exists(device))
        log(QStringLiteral("  port %1 bound").arg(device));
    else
        log(QStringLiteral("  warning: %1 was not created — is Bluetooth up?").arg(device));

    const QString target = cupsBackendDir() + QStringLiteral("/rfcomm");
    log(QStringLiteral("CUPS backend %1").arg(target));
    if (!QFileInfo::exists(target))
        log(QStringLiteral("  missing — was the package installed completely?"));

    // Fall back to the Bluetooth name for the model, otherwise the driver lookup
    // picks something unrelated.
    const QString deviceName = model.isEmpty() ? deviceNameFor(mac) : model;
    const DriverMatch driver = findDriver(deviceName);
    if (driver.ppd.isEmpty()) {
        log(QStringLiteral("No P-touch driver found — please install printer-driver-ptouch "
                           "(or ptouch-driver)."));
        return 2;
    }
    if (driver.model.isEmpty()) {
        log(QStringLiteral("Warning: no driver for \"%1\" — using a generic P-touch driver. "
                           "Tape widths may differ.").arg(deviceName));
    }
    const QString resolvedModel = driver.model.isEmpty() ? model : driver.model;
    log(QStringLiteral("queue %1 using %2").arg(queue, driver.ppd));
    const QString lpadmin = run(
        QStringLiteral("lpadmin"),
        {QStringLiteral("-p"), queue, QStringLiteral("-E"),
         QStringLiteral("-v"), QStringLiteral("rfcomm:%1").arg(device),
         QStringLiteral("-m"), driver.ppd,
         QStringLiteral("-D"), QStringLiteral("Brother %1 (Bluetooth)")
                                   .arg(resolvedModel.isEmpty() ? QStringLiteral("P-touch")
                                                                : resolvedModel),
         QStringLiteral("-L"), QStringLiteral("Bluetooth")}, &code);
    if (code != 0) {
        log(QStringLiteral("  lpadmin: %1").arg(lpadmin.trimmed()));
        return 3;
    }
    run(QStringLiteral("cupsenable"), {queue});
    run(QStringLiteral("cupsaccept"), {queue});

    log(QStringLiteral("Done."));
    return 0;
}

} // namespace ptouch
