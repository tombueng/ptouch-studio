#include "Cli.h"

#include "Config.h"
#include "Engine.h"
#include "Printer.h"
#include "Provision.h"
#include "Status.h"
#include "Version.h"

#include <QCommandLineParser>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTextStream>
#include <QThread>

#include <iostream>
#include <unistd.h>

namespace ptouch {
namespace {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

Align alignFromString(const QString &value)
{
    if (value.startsWith(QStringLiteral("l"), Qt::CaseInsensitive)) return Align::Left;
    if (value.startsWith(QStringLiteral("r"), Qt::CaseInsensitive)) return Align::Right;
    return Align::Center;
}

int cmdStatus(const QStringList &args)
{
    QCommandLineParser p;
    p.addOption({{QStringLiteral("j"), QStringLiteral("json")},
                 QStringLiteral("print as JSON")});
    p.addOption({{QStringLiteral("d"), QStringLiteral("device")},
                 QStringLiteral("RFCOMM port"), QStringLiteral("path")});
    p.addHelpOption();
    p.process(args);

    const Config cfg = Config::load();
    const QString device = p.isSet(QStringLiteral("device"))
                               ? p.value(QStringLiteral("device")) : cfg.device;
    const Status s = queryStatusRetry(device);

    if (p.isSet(QStringLiteral("json"))) {
        QJsonObject o;
        o[QStringLiteral("ok")] = s.ok;
        o[QStringLiteral("tape_mm")] = s.tapeMm;
        o[QStringLiteral("media_type")] = s.mediaType;
        o[QStringLiteral("phase")] = s.phase;
        if (!s.error.isEmpty())
            o[QStringLiteral("error")] = s.error;
        o[QStringLiteral("errors")] = QJsonArray::fromStringList(s.errors);
        out() << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << Qt::endl;
        return s.ok ? 0 : 1;
    }

    if (!s.error.isEmpty()) {
        err() << QStringLiteral("Error: ") << s.error << Qt::endl;
        return 1;
    }
    out() << QStringLiteral("Tape   : %1 mm").arg(s.tapeMm) << Qt::endl;
    out() << QStringLiteral("Media  : %1").arg(s.mediaType) << Qt::endl;
    out() << QStringLiteral("Status : %1")
                 .arg(s.ok ? QStringLiteral("ready")
                           : (s.errors.isEmpty() ? QStringLiteral("not ready")
                                                 : s.errors.join(QStringLiteral(", "))))
          << Qt::endl;
    return s.ok ? 0 : 1;
}

int cmdPrint(const QStringList &args)
{
    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("Print a label"));
    p.addPositionalArgument(QStringLiteral("text"),
                            QStringLiteral("text; each argument becomes one line"),
                            QStringLiteral("[text …]"));
    p.addOption({{QStringLiteral("w"), QStringLiteral("width")},
                 QStringLiteral("tape width in mm (default: ask the printer)"),
                 QStringLiteral("mm")});
    p.addOption({{QStringLiteral("s"), QStringLiteral("size")},
                 QStringLiteral("font size in pt (default: fill the tape height)"),
                 QStringLiteral("pt")});
    p.addOption({{QStringLiteral("l"), QStringLiteral("length")},
                 QStringLiteral("fixed label length in mm"), QStringLiteral("mm")});
    p.addOption({{QStringLiteral("a"), QStringLiteral("align")},
                 QStringLiteral("left | center | right"), QStringLiteral("value"),
                 QStringLiteral("center")});
    p.addOption({{QStringLiteral("m"), QStringLiteral("margin")},
                 QStringLiteral("side margin in mm"), QStringLiteral("mm"),
                 QStringLiteral("3")});
    p.addOption({{QStringLiteral("n"), QStringLiteral("copies")},
                 QStringLiteral("number of copies"), QStringLiteral("count"),
                 QStringLiteral("1")});
    p.addOption({{QStringLiteral("f"), QStringLiteral("font")},
                 QStringLiteral("font family"), QStringLiteral("name"),
                 QStringLiteral("DejaVu Sans")});
    p.addOption({QStringLiteral("plain"), QStringLiteral("do not print bold")});
    p.addOption({QStringLiteral("italic"), QStringLiteral("italic")});
    p.addOption({QStringLiteral("frame"), QStringLiteral("draw a frame around the label")});
    p.addOption({QStringLiteral("mirror"),
                 QStringLiteral("mirrored (tape read from behind)")});
    p.addOption({QStringLiteral("no-cut"), QStringLiteral("do not cut automatically")});
    p.addOption({QStringLiteral("pdf"), QStringLiteral("write a PDF instead of printing"),
                 QStringLiteral("file")});
    p.addHelpOption();
    p.process(args);

    // The sub-command sits in args[0] and counts as the program name to the parser,
    // so the positional arguments are already just the lines of text.
    const QStringList text = p.positionalArguments();
    if (text.isEmpty()) {
        err() << QStringLiteral("No text given.") << Qt::endl;
        return 2;
    }

    const Config cfg = Config::load();
    Spec spec;
    spec.text = text.join(QLatin1Char('\n'));
    spec.family = p.value(QStringLiteral("font"));
    spec.bold = !p.isSet(QStringLiteral("plain"));
    spec.italic = p.isSet(QStringLiteral("italic"));
    spec.align = alignFromString(p.value(QStringLiteral("align")));
    spec.marginMm = p.value(QStringLiteral("margin")).toDouble();
    spec.copies = std::max(1, p.value(QStringLiteral("copies")).toInt());
    spec.frame = p.isSet(QStringLiteral("frame"));
    spec.mirror = p.isSet(QStringLiteral("mirror"));
    spec.autocut = !p.isSet(QStringLiteral("no-cut"));

    if (p.isSet(QStringLiteral("size"))) {
        spec.autoSize = false;
        spec.sizePt = p.value(QStringLiteral("size")).toDouble();
    }
    if (p.isSet(QStringLiteral("length"))) {
        spec.autoLength = false;
        spec.lengthMm = p.value(QStringLiteral("length")).toDouble();
    }

    double width = 0;
    if (p.isSet(QStringLiteral("width"))) {
        width = p.value(QStringLiteral("width")).toDouble();
    } else {
        const Status s = queryStatusRetry(cfg.device, 2);
        if (s.tapeMm > 0) {
            width = s.tapeMm;
        } else {
            err() << QStringLiteral("Cannot read the tape width (%1) — assuming 12 mm")
                         .arg(s.error) << Qt::endl;
            width = 12.0;
        }
    }
    if (!tapeFor(width)) {
        double nearest = 12.0;
        for (const Tape &t : tapes())
            if (qAbs(t.widthMm - width) < qAbs(nearest - width))
                nearest = t.widthMm;
        err() << QStringLiteral("%1 mm is unknown, using %2 mm").arg(width).arg(nearest)
              << Qt::endl;
        width = nearest;
    }
    spec.tapeMm = width;

    const Layout layout = computeLayout(spec);
    for (const QString &w : layout.warnings)
        err() << QStringLiteral("Note: ") << w << Qt::endl;

    if (p.isSet(QStringLiteral("pdf"))) {
        const QString path = p.value(QStringLiteral("pdf"));
        writePdf(path, spec, layout);
        out() << QStringLiteral("%1: %2 × %3 mm, font %4 pt")
                     .arg(path)
                     .arg(layout.lengthPt / PtPerMm, 0, 'f', 0)
                     .arg(width, 0, 'g', 3)
                     .arg(layout.sizePt, 0, 'f', 1) << Qt::endl;
        return 0;
    }

    const PrintResult r = printAndWait(spec, layout, cfg.printer);
    if (!r.ok) {
        err() << r.message << Qt::endl;
        return 1;
    }
    out() << QStringLiteral("%1 — %2 × %3 mm, font %4 pt")
                 .arg(r.message)
                 .arg(layout.lengthPt / PtPerMm, 0, 'f', 0)
                 .arg(width, 0, 'g', 3)
                 .arg(layout.sizePt, 0, 'f', 1) << Qt::endl;
    return 0;
}

int cmdCheck()
{
    const SetupState s = checkSetup();
    const auto mark = [](bool ok) { return ok ? QStringLiteral("✓") : QStringLiteral("✗"); };
    out() << mark(s.dependenciesOk()) << QStringLiteral(" system tools") << Qt::endl;
    out() << mark(s.paired) << QStringLiteral(" printer paired %1").arg(s.mac) << Qt::endl;
    out() << mark(s.rfcommActive) << QStringLiteral(" RFCOMM service") << Qt::endl;
    out() << mark(s.deviceAccessible) << QStringLiteral(" access to %1").arg(s.device)
          << Qt::endl;
    out() << mark(s.cupsBackend) << QStringLiteral(" CUPS backend") << Qt::endl;
    out() << mark(s.queuePresent) << QStringLiteral(" print queue %1").arg(s.queue)
          << Qt::endl;
    const QStringList todo = s.missingSteps();
    if (!todo.isEmpty()) {
        out() << Qt::endl << QStringLiteral("Still to do:") << Qt::endl;
        for (const QString &t : todo)
            out() << QStringLiteral("  · ") << t << Qt::endl;
    }
    return s.ready() ? 0 : 1;
}

int cmdScan()
{
    out() << QStringLiteral("Searching (12 s) …") << Qt::endl;
    const QList<Device> devices = scanForPrinters(12);
    for (const Device &d : devices)
        out() << QStringLiteral("%1  %2").arg(d.mac, d.name) << Qt::endl;
    if (devices.isEmpty()) {
        err() << QStringLiteral("No P-touch found.") << Qt::endl;
        return 1;
    }
    return 0;
}

int cmdSetup(const QStringList &args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("mac"), QStringLiteral("Bluetooth address"),
                 QStringLiteral("address")});
    p.addOption({QStringLiteral("queue"), QStringLiteral("name of the print queue"),
                 QStringLiteral("name"), QString::fromLatin1(DefaultQueue)});
    p.addHelpOption();
    p.process(args);

    Config cfg = Config::load();
    QString mac = p.value(QStringLiteral("mac"));
    QString model = cfg.model;

    if (mac.isEmpty() && !cfg.mac.isEmpty())
        mac = cfg.mac;
    if (mac.isEmpty()) {
        out() << QStringLiteral("Searching for printers (12 s) …") << Qt::endl;
        const QList<Device> devices = scanForPrinters(12);
        if (devices.isEmpty()) {
            err() << QStringLiteral("No P-touch found. Switch the printer on.") << Qt::endl;
            return 1;
        }
        for (int i = 0; i < devices.size(); ++i)
            out() << QStringLiteral("  %1) %2  %3").arg(i + 1).arg(devices.at(i).mac,
                                                                  devices.at(i).name)
                  << Qt::endl;
        int choice = 1;
        if (devices.size() > 1) {
            out() << QStringLiteral("Pick one [1-%1]: ").arg(devices.size());
            out().flush();
            std::string line;
            std::getline(std::cin, line);
            choice = QString::fromStdString(line).trimmed().toInt();
            if (choice < 1 || choice > devices.size())
                choice = 1;
        }
        mac = devices.at(choice - 1).mac;
        model = devices.at(choice - 1).model;
        out() << QStringLiteral("-> %1 (%2)").arg(mac, model) << Qt::endl;
    }

    const SetupState before = checkSetup();
    if (!before.paired) {
        out() << QStringLiteral("Pairing …") << Qt::endl;
        QString log;
        if (!pairDevice(mac, &log)) {
            err() << log << Qt::endl << QStringLiteral("Pairing failed.") << Qt::endl;
            return 1;
        }
    }

    const QString queue = p.value(QStringLiteral("queue"));
    QStringList command = systemSetupCommand(mac, model, queue,
                                             qEnvironmentVariable("USER"));
    out() << QStringLiteral("System setup (needs administrator rights) …") << Qt::endl;

    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    if (::geteuid() == 0) {
        const QString program = command.takeFirst();
        process.start(program, command);
    } else {
        process.start(QStringLiteral("sudo"), command);
    }
    process.waitForFinished(300000);
    if (process.exitCode() != 0)
        return process.exitCode();

    cfg.printer = queue;
    cfg.mac = mac;
    cfg.model = model;
    cfg.save();
    out() << QStringLiteral("Configuration saved: %1").arg(Config::configFilePath())
          << Qt::endl;
    return cmdCheck();
}

int cmdSetupSystem(const QStringList &args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("mac"), QStringLiteral("address"), QStringLiteral("mac")});
    p.addOption({QStringLiteral("model"), QStringLiteral("model"), QStringLiteral("model"),
                 QString()});
    p.addOption({QStringLiteral("queue"), QStringLiteral("print queue"),
                 QStringLiteral("name"), QString::fromLatin1(DefaultQueue)});
    p.addOption({QStringLiteral("index"), QStringLiteral("RFCOMM index"),
                 QStringLiteral("n"), QStringLiteral("0")});
    p.addOption({QStringLiteral("channel"), QStringLiteral("RFCOMM channel"),
                 QStringLiteral("n"), QStringLiteral("1")});
    p.addOption({QStringLiteral("owner"), QStringLiteral("user allowed to use the port"),
                 QStringLiteral("name"), QString()});
    p.addHelpOption();
    p.process(args);

    if (p.value(QStringLiteral("mac")).isEmpty()) {
        err() << QStringLiteral("--mac is missing") << Qt::endl;
        return 2;
    }
    return installSystem(p.value(QStringLiteral("mac")), p.value(QStringLiteral("model")),
                         p.value(QStringLiteral("queue")),
                         p.value(QStringLiteral("index")).toInt(),
                         p.value(QStringLiteral("channel")).toInt(),
                         p.value(QStringLiteral("owner")),
                         [](const QString &line) { out() << line << Qt::endl; out().flush(); });
}

} // namespace

QString usage()
{
    return QStringLiteral(
        "%1 %2 — labels for Brother P-touch tape cassettes\n"
        "\n"
        "Usage:\n"
        "  ptouch-studio                     start the interface\n"
        "  ptouch-studio print \"text\" …      print a label\n"
        "  ptouch-studio status [--json]     loaded tape and readiness\n"
        "  ptouch-studio scan                look for P-touch devices\n"
        "  ptouch-studio setup               set up the printer\n"
        "  ptouch-studio check               check the setup\n"
        "\n"
        "\"ptouch-studio print --help\" lists the layout options.\n")
        .arg(QStringLiteral(PTOUCH_APP_NAME), QStringLiteral(PTOUCH_VERSION));
}

int runCli(const QStringList &arguments)
{
    const QString command = arguments.value(1);

    if (command == QStringLiteral("status"))       return cmdStatus(arguments.mid(1));
    if (command == QStringLiteral("print"))        return cmdPrint(arguments.mid(1));
    if (command == QStringLiteral("check"))        return cmdCheck();
    if (command == QStringLiteral("scan"))         return cmdScan();
    if (command == QStringLiteral("setup"))        return cmdSetup(arguments.mid(1));
    if (command == QStringLiteral("setup-system")) return cmdSetupSystem(arguments.mid(1));

    out() << usage();
    return command.isEmpty() || command == QStringLiteral("--help")
           || command == QStringLiteral("-h") ? 0 : 2;
}

} // namespace ptouch
