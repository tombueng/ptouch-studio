#include "Log.h"

#include "Config.h"
#include "Provision.h"
#include "Version.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QProcess>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>
#include <QtGlobal>

namespace ptouch {
namespace log {
namespace {

QMutex g_mutex;
QString g_path;
bool g_ready = false;

constexpr qint64 RotateBytes = 1 << 20;   // 1 MiB — keep one older generation

QString resolvePath()
{
    // GenericStateLocation (~/.local/state) is where a session log belongs, but it
    // only arrived in Qt 6.7. On older Qt — e.g. the 6.2 on Ubuntu 22.04 that the
    // CI and the AppImage build on — fall back to the XDG data dir, same ~/.local
    // layout, so the log still lands in a sensible per-user place.
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
#else
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#endif
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath();
    dir += QStringLiteral("/ptouch-studio");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/session.log");
}

void writeRaw(const QString &text)
{
    if (g_path.isEmpty())
        return;
    QFile f(g_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    QTextStream out(&f);
    out << text;
}

QString stamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

// Runs a helper and returns its combined output, or an empty string. Never throws,
// never blocks for long — diagnostics must not hang on an unresponsive tool.
QString run(const QString &program, const QStringList &args, int timeoutMs = 8000)
{
    if (QStandardPaths::findExecutable(program).isEmpty())
        return {};
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        return QString::fromLocal8Bit(p.readAll()) + QStringLiteral("\n[timed out]\n");
    }
    return QString::fromLocal8Bit(p.readAll());
}

QString shell(const QString &command, int timeoutMs = 8000)
{
    return run(QStringLiteral("sh"), {QStringLiteral("-c"), command}, timeoutMs);
}

// Last `maxLines` lines of a file; notes when it cannot be read (permissions).
QString tailFile(const QString &path, int maxLines)
{
    QFile f(path);
    if (!f.exists())
        return QStringLiteral("(not present)");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringLiteral("(exists but not readable: %1)").arg(f.errorString());
    QStringList lines;
    QTextStream in(&f);
    while (!in.atEnd()) {
        lines << in.readLine();
        if (lines.size() > maxLines * 4)       // keep memory bounded on huge logs
            lines.removeFirst();
    }
    if (lines.size() > maxLines)
        lines = lines.mid(lines.size() - maxLines);
    return lines.join(QLatin1Char('\n'));
}

QString osPrettyName()
{
    QFile f(QStringLiteral("/etc/os-release"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QString l = in.readLine();
            if (l.startsWith(QStringLiteral("PRETTY_NAME=")))
                return l.mid(12).remove(QLatin1Char('"'));
        }
    }
    return QSysInfo::prettyProductName();
}

// Pulls the telling lines out of a queue's PPD without dumping the whole file.
QString ppdSummary(const QString &queue)
{
    if (queue.isEmpty())
        return {};
    const QString path = QStringLiteral("/etc/cups/ppd/%1.ppd").arg(queue);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringLiteral("%1: %2").arg(path, f.errorString());
    QStringList keep;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString l = in.readLine();
        if (l.startsWith(QStringLiteral("*NickName")) || l.startsWith(QStringLiteral("*ModelName"))
            || l.startsWith(QStringLiteral("*Product")) || l.startsWith(QStringLiteral("*cupsFilter"))
            || l.startsWith(QStringLiteral("*cupsModelNumber"))
            || l.startsWith(QStringLiteral("*DefaultResolution"))
            || l.startsWith(QStringLiteral("*DefaultPageSize"))
            || l.startsWith(QStringLiteral("*FoomaticRIPCommandLine")))
            keep << l;
    }
    return keep.isEmpty() ? QStringLiteral("(%1: no telling lines)").arg(path)
                          : keep.join(QLatin1Char('\n'));
}

} // namespace

QString filePath()
{
    return g_path.isEmpty() ? resolvePath() : g_path;
}

void init(const QString &invocation)
{
    QMutexLocker lock(&g_mutex);
    if (g_ready)
        return;
    g_path = resolvePath();

    // Keep one previous generation so a report still has the run before this one.
    if (QFileInfo(g_path).size() > RotateBytes) {
        const QString old = g_path + QStringLiteral(".1");
        QFile::remove(old);
        QFile::rename(g_path, old);
    }
    g_ready = true;
    lock.unlock();

    section(QStringLiteral("%1 %2 started").arg(QStringLiteral(PTOUCH_APP_NAME),
                                                QStringLiteral(PTOUCH_VERSION)));
    field(QStringLiteral("command"), invocation);
    field(QStringLiteral("Qt"), QString::fromLatin1(qVersion()));
    field(QStringLiteral("system"), osPrettyName() + QStringLiteral(" / ")
                                        + QSysInfo::kernelType() + QLatin1Char(' ')
                                        + QSysInfo::kernelVersion());
}

void line(const QString &message)
{
    QMutexLocker lock(&g_mutex);
    writeRaw(stamp() + QLatin1Char(' ') + message + QLatin1Char('\n'));
}

void field(const QString &key, const QString &value)
{
    line(key + QStringLiteral(": ") + value);
}

void section(const QString &title)
{
    QMutexLocker lock(&g_mutex);
    writeRaw(QLatin1Char('\n') + stamp()
             + QStringLiteral(" ===== ") + title + QStringLiteral(" =====\n"));
}

void block(const QString &caption, const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return;
    QString indented;
    const auto rows = trimmed.split(QLatin1Char('\n'));
    for (const QString &r : rows)
        indented += QStringLiteral("    ") + r + QLatin1Char('\n');
    QMutexLocker lock(&g_mutex);
    if (!caption.isEmpty())
        writeRaw(stamp() + QStringLiteral(" --- ") + caption + QStringLiteral(" ---\n"));
    writeRaw(indented);
}

QString sessionText()
{
    QMutexLocker lock(&g_mutex);
    QFile f(g_path.isEmpty() ? resolvePath() : g_path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

QString collectDiagnostics()
{
    const Config cfg = Config::load();
    const SetupState state = checkSetup();

    QString r;
    QTextStream o(&r);
    const auto head = [&o](const QString &t) {
        o << "\n========== " << t << " ==========\n";
    };

    o << "P-touch Studio diagnostics report\n";
    o << "generated: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    o << "version  : " << PTOUCH_VERSION << "\n";
    o << "Qt       : " << qVersion() << "\n";
    o << "system   : " << osPrettyName() << " / " << QSysInfo::kernelType() << " "
      << QSysInfo::kernelVersion() << " (" << QSysInfo::currentCpuArchitecture() << ")\n";
    o << "appimage : " << (qEnvironmentVariableIsSet("APPIMAGE")
                               ? qEnvironmentVariable("APPIMAGE") : QStringLiteral("no")) << "\n";

    head(QStringLiteral("configuration"));
    o << "queue    : " << cfg.printer << "\n";
    o << "device   : " << cfg.device << "\n";
    o << "mac      : " << cfg.mac << "\n";
    o << "model    : " << cfg.model << "\n";
    o << "config   : " << Config::configFilePath() << "\n";
    o << "rfcomm.conf (" << SystemConfPath << "):\n" << tailFile(QString::fromLatin1(SystemConfPath), 40) << "\n";

    head(QStringLiteral("setup self-check"));
    o << "dependencies ok : " << (state.dependenciesOk() ? "yes" : "no") << "\n";
    o << "paired          : " << (state.paired ? "yes" : "no") << "\n";
    o << "RFCOMM service  : " << (state.rfcommActive ? "running" : "not running") << "\n";
    o << "port accessible : " << (state.deviceAccessible ? "yes" : "no") << " (" << state.device << ")\n";
    o << "CUPS backend    : " << (state.cupsBackend ? "installed" : "missing") << "\n";
    o << "queue present   : " << (state.queuePresent ? "yes" : "no") << " (" << state.queue << ")\n";
    if (!state.missingSteps().isEmpty())
        o << "still to do     : " << state.missingSteps().join(QStringLiteral(", ")) << "\n";

    head(QStringLiteral("CUPS queues (lpstat -t)"));
    o << run(QStringLiteral("lpstat"), {QStringLiteral("-t")}) << "\n";

    const QString queue = cfg.printer.isEmpty() ? findPtouchQueue() : cfg.printer;
    head(QStringLiteral("queue detail: ") + queue);
    o << run(QStringLiteral("lpstat"), {QStringLiteral("-l"), QStringLiteral("-p"), queue}) << "\n";
    o << "options:\n"
      << run(QStringLiteral("lpoptions"), {QStringLiteral("-p"), queue, QStringLiteral("-l")}) << "\n";
    o << "PPD:\n" << ppdSummary(queue) << "\n";

    head(QStringLiteral("available P-touch drivers (lpinfo -m | ptouch)"));
    o << "(this is the list the setup matches the printer model against)\n";
    o << shell(QStringLiteral("lpinfo -m 2>&1 | grep -i ptouch")) << "\n";

    head(QStringLiteral("installed packages"));
    // Gate pacman on /etc/pacman.conf, not `command -v pacman`: on Debian/Ubuntu
    // the latter finds /usr/games/pacman (the arcade game), and `pacman -Q` then
    // launches it. dpkg-query -W is used over `dpkg -l` because it is far faster.
    o << shell(QStringLiteral(
        "test -f /etc/pacman.conf && pacman -Q 2>/dev/null | grep -Ei "
        "'ptouch|cups|bluez|foomatic|ghostscript'; "
        "test -f /var/lib/rpm/rpmdb.sqlite -o -f /var/lib/rpm/Packages "
        "&& rpm -qa 2>/dev/null | grep -Ei 'ptouch|cups|bluez|foomatic'; "
        "command -v dpkg-query >/dev/null && dpkg-query -W -f='${Package} ${Version}\\n' 2>/dev/null "
        "| grep -Ei 'ptouch|cups|bluez|foomatic'"),
        20000)
      << "\n";

    head(QStringLiteral("RFCOMM / Bluetooth"));
    o << shell(QStringLiteral("ls -l /dev/rfcomm* 2>&1")) << "\n";
    o << run(QStringLiteral("rfcomm"), {QStringLiteral("show")}) << "\n";
    o << "rfcomm binary: " << shell(QStringLiteral("command -v rfcomm || echo 'NOT FOUND "
                                                   "(Arch: install bluez-deprecated-tools)'"));
    if (!cfg.mac.isEmpty())
        o << "bluetoothctl info:\n"
          << run(QStringLiteral("bluetoothctl"), {QStringLiteral("info"), cfg.mac}) << "\n";

    head(QStringLiteral("ptouch-rfcomm.service"));
    o << run(QStringLiteral("systemctl"),
             {QStringLiteral("status"), QStringLiteral("ptouch-rfcomm.service"),
              QStringLiteral("--no-pager"), QStringLiteral("-l")}) << "\n";

    head(QStringLiteral("CUPS backend binary"));
    o << shell(QStringLiteral("ls -l /usr/lib/cups/backend/rfcomm "
                              "/usr/libexec/cups/backend/rfcomm 2>&1")) << "\n";

    head(QStringLiteral("privileged backend log (") + QString::fromLatin1(BackendLogPath) + QStringLiteral(")"));
    o << "(written by the CUPS backend as it talks to the printer — bytes sent and "
         "every status reply)\n";
    o << tailFile(QString::fromLatin1(BackendLogPath), 200) << "\n";

    head(QStringLiteral("CUPS error_log (tail)"));
    o << tailFile(QStringLiteral("/var/log/cups/error_log"), 200) << "\n";

    head(QStringLiteral("session log (") + filePath() + QStringLiteral(")"));
    o << sessionText() << "\n";

    o << "\n========== end of report ==========\n";
    return r;
}

} // namespace log
} // namespace ptouch
