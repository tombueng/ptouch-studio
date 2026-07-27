#include "Config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

namespace ptouch {
namespace {

QString runCommand(const QString &program, const QStringList &args, int timeoutMs = 10000)
{
    QProcess p;
    p.start(program, args);
    if (!p.waitForFinished(timeoutMs))
        p.kill();
    return QString::fromLocal8Bit(p.readAllStandardOutput());
}

} // namespace

QString Config::configFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + QStringLiteral("/config.json");
}

QString systemConfValue(const QString &key)
{
    QFile f(QString::fromLatin1(SystemConfPath));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith(QLatin1Char('#')) || !line.contains(QLatin1Char('=')))
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (line.left(eq).trimmed() == key)
            return line.mid(eq + 1).trimmed().remove(QLatin1Char('"'));
    }
    return {};
}

QString findPtouchQueue()
{
    const QString out = runCommand(QStringLiteral("lpstat"), {QStringLiteral("-v")});
    // "device for NAME: uri", localised installations use their own wording
    static const QRegularExpression re(QStringLiteral("(?:für|for)\\s+(\\S+?):"));
    QStringList queues;
    auto it = re.globalMatch(out);
    while (it.hasNext())
        queues << it.next().captured(1);

    for (const QString &q : std::as_const(queues)) {
        const QString opts = runCommand(QStringLiteral("lpoptions"),
                                        {QStringLiteral("-p"), q, QStringLiteral("-l")});
        if (opts.contains(QStringLiteral("AutoCut")))   // marks the ptouch driver
            return q;
    }
    return queues.value(0);
}

Config Config::load()
{
    Config c;

    QFile f(configFilePath());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        c.printer = o.value(QStringLiteral("printer")).toString();
        c.device = o.value(QStringLiteral("device")).toString(c.device);
        c.mac = o.value(QStringLiteral("mac")).toString();
        c.model = o.value(QStringLiteral("model")).toString();
    }

    if (c.mac.isEmpty())
        c.mac = systemConfValue(QStringLiteral("PTOUCH_MAC"));
    const QString sysDevice = systemConfValue(QStringLiteral("PTOUCH_DEVICE"));
    if (!sysDevice.isEmpty() && c.device.isEmpty())
        c.device = sysDevice;

    const auto env = [](const char *name) { return qEnvironmentVariable(name); };
    if (!env("PTOUCH_PRINTER").isEmpty())
        c.printer = env("PTOUCH_PRINTER");
    if (!env("PTOUCH_DEVICE").isEmpty())
        c.device = env("PTOUCH_DEVICE");
    if (!env("PTOUCH_MAC").isEmpty())
        c.mac = env("PTOUCH_MAC");

    if (c.printer.isEmpty())
        c.printer = findPtouchQueue();
    return c;
}

void Config::save() const
{
    const QString path = configFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject o;
    o[QStringLiteral("printer")] = printer;
    o[QStringLiteral("device")] = device;
    o[QStringLiteral("mac")] = mac;
    o[QStringLiteral("model")] = model;

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(o).toJson());
}

} // namespace ptouch
