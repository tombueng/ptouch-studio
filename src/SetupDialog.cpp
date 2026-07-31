#include "SetupDialog.h"

#include "Config.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace ptouch {

// Searches on a thread and hands the result back to the dialog.
class SetupDialog::ScanWorker : public QThread {
    Q_OBJECT
public:
    explicit ScanWorker(QObject *parent = nullptr) : QThread(parent) {}
    QList<UsbPrinter> usb;
    QList<Device> bluetooth;

protected:
    void run() override
    {
        usb = scanUsbPrinters();          // instant
        bluetooth = scanForPrinters(12);  // the slow part
    }
};

SetupDialog::SetupDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Set up the printer"));
    resize(660, 620);

    auto *v = new QVBoxLayout(this);

    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    m_summary->setTextFormat(Qt::RichText);
    v->addWidget(m_summary);

    m_steps = new QLabel;
    m_steps->setWordWrap(true);
    m_steps->setTextFormat(Qt::RichText);
    v->addWidget(m_steps);

    auto *deviceGroup = new QGroupBox(tr("1. Find the printer"));
    auto *deviceLayout = new QVBoxLayout(deviceGroup);
    auto *row = new QHBoxLayout;
    m_deviceBox = new QComboBox;
    m_deviceBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_deviceBox->setPlaceholderText(tr("not searched yet"));
    m_scanButton = new QPushButton(tr("Search"));
    m_pairButton = new QPushButton(tr("Pair"));
    connect(m_scanButton, &QPushButton::clicked, this, &SetupDialog::scan);
    connect(m_pairButton, &QPushButton::clicked, this, &SetupDialog::pair);
    row->addWidget(m_deviceBox, 1);
    row->addWidget(m_scanButton);
    row->addWidget(m_pairButton);
    deviceLayout->addLayout(row);
    deviceLayout->addWidget(new QLabel(
        QStringLiteral("USB printers are found instantly and need no pairing. For "
                       "Bluetooth the printer has to be switched on; an existing "
                       "connection to a phone does not get in the way.")));
    v->addWidget(deviceGroup);

    auto *systemGroup = new QGroupBox(
        tr("2. Set up the port and the print queue"));
    auto *systemLayout = new QVBoxLayout(systemGroup);
    systemLayout->addWidget(new QLabel(
        QStringLiteral("Creates the access rule and the print queue — over Bluetooth "
                       "also the RFCOMM service and the CUPS backend. This asks for the "
                       "administrator password once.")));
    m_systemButton = new QPushButton(tr("Set up now"));
    connect(m_systemButton, &QPushButton::clicked, this, &SetupDialog::runSystemSetup);
    systemLayout->addWidget(m_systemButton);
    v->addWidget(systemGroup);

    m_progress = new QProgressBar;
    m_progress->setRange(0, 0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_progress->hide();
    v->addWidget(m_progress);

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500);
    v->addWidget(m_log, 1);

    auto *buttons = new QDialogButtonBox;
    m_recheckButton = buttons->addButton(tr("Check again"),
                                         QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    connect(m_recheckButton, &QPushButton::clicked, this, &SetupDialog::recheck);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    v->addWidget(buttons);

    recheck();
}

SetupDialog::Choice SetupDialog::currentChoice() const
{
    const int index = m_deviceBox->currentIndex();
    if (index < 0)
        return {};
    // USB entries come first in the list.
    if (index < m_usbPrinters.size()) {
        const UsbPrinter &p = m_usbPrinters.at(index);
        return {true, QString(), p.uri, p.model, p.devicePath};
    }
    const int bluetooth = index - m_usbPrinters.size();
    if (bluetooth >= m_devices.size())
        return {};
    const Device &d = m_devices.at(bluetooth);
    return {false, d.mac, QString(), d.model, QString()};
}

void SetupDialog::appendLog(const QString &line)
{
    m_log->appendPlainText(line);
}

void SetupDialog::setWorking(bool working, const QString &what)
{
    m_progress->setVisible(working);
    m_scanButton->setEnabled(!working);
    m_pairButton->setEnabled(!working);
    m_systemButton->setEnabled(!working);
    m_recheckButton->setEnabled(!working);
    if (working && !what.isEmpty())
        appendLog(what);

    // The override cursor is a stack: pushing one on the way out and popping it
    // in the same breath leaves the wait cursor behind for good.
    if (working == m_working)
        return;
    m_working = working;
    if (working)
        QApplication::setOverrideCursor(Qt::WaitCursor);
    else
        QApplication::restoreOverrideCursor();
}

void SetupDialog::recheck()
{
    m_state = checkSetup();
    updateView();
}

void SetupDialog::updateView()
{
    const auto mark = [](bool ok) {
        return ok ? tr("<span style='color:#1e8449'>✓</span>")
                  : tr("<span style='color:#c0392b'>✗</span>");
    };

    if (m_state.ready()) {
        m_summary->setText(QStringLiteral(
            "<h3 style='color:#1e8449'>Everything is set up</h3>"
            "<p>Queue <b>%1</b>, port <b>%2</b>.</p>").arg(m_state.queue, m_state.device));
    } else {
        m_summary->setText(tr("<h3>Setup incomplete</h3>"));
    }

    QStringList lines;
    lines << tr("%1 system tools (bluez, cups, ptouch driver)")
                 .arg(mark(m_state.dependenciesOk()));
    lines << tr("%1 printer paired%2").arg(mark(m_state.paired),
              m_state.mac.isEmpty() ? QString() : tr(" (%1)").arg(m_state.mac));
    lines << tr("%1 RFCOMM service running").arg(mark(m_state.rfcommActive));
    lines << tr("%1 access to %2").arg(mark(m_state.deviceAccessible),
                                                   m_state.device);
    lines << tr("%1 CUPS backend installed").arg(mark(m_state.cupsBackend));
    lines << tr("%1 print queue %2").arg(mark(m_state.queuePresent),
              m_state.queue.isEmpty() ? tr("—") : m_state.queue);

    const QStringList todo = m_state.missingSteps();
    if (!todo.isEmpty())
        lines << tr("<br><b>Still to do:</b> %1").arg(todo.join(tr(", ")));

    m_steps->setText(lines.join(QStringLiteral("<br>")));
    m_pairButton->setEnabled(m_deviceBox->count() > 0);
}

void SetupDialog::scan()
{
    setWorking(true, tr("Searching for P-touch devices (about 12 seconds) …"));
    QApplication::processEvents();

    m_deviceBox->clear();

    // USB is instant and needs no pairing, so look there first.
    m_usbPrinters = scanUsbPrinters();
    for (const UsbPrinter &p : std::as_const(m_usbPrinters)) {
        m_deviceBox->addItem(tr("USB — %1").arg(p.model), p.uri);
        appendLog(tr("found on USB: %1 (%2)").arg(p.model, p.devicePath));
    }

    m_devices = scanForPrinters(12);
    for (const Device &d : std::as_const(m_devices)) {
        m_deviceBox->addItem(tr("Bluetooth — %1 (%2)").arg(d.name, d.mac), d.mac);
        appendLog(tr("found on Bluetooth: %1 (%2)").arg(d.name, d.mac));
    }

    if (m_devices.isEmpty() && m_usbPrinters.isEmpty())
        appendLog(QStringLiteral("No P-touch found. Switch the printer on, plug it in or "
                                 "bring it into range."));
    setWorking(false);
    updateView();
}

void SetupDialog::pair()
{
    const Choice choice = currentChoice();
    if (!choice.valid())
        return;
    if (choice.usb) {
        appendLog(tr("A USB printer needs no pairing — go straight to step 2."));
        return;
    }
    const Device device{choice.mac, QString(), choice.model};

    setWorking(true, tr("Pairing with %1 …").arg(device.mac));
    QApplication::processEvents();

    QString output;
    const bool ok = pairDevice(device.mac, &output);
    if (!output.isEmpty())
        appendLog(output);
    appendLog(ok ? tr("Pairing succeeded.") : tr("Pairing failed."));

    if (ok) {
        Config cfg = Config::load();
        cfg.mac = device.mac;
        cfg.model = device.model;
        cfg.save();
    }
    setWorking(false);
    recheck();
}

void SetupDialog::runSystemSetup()
{
    Config cfg = Config::load();
    Choice choice = currentChoice();
    if (!choice.valid() && !cfg.mac.isEmpty())
        choice = {false, cfg.mac, QString(), cfg.model, QString()};
    if (!choice.valid()) {
        appendLog(tr("Search for a printer first."));
        return;
    }

    const QString queue = cfg.printer.isEmpty() ? QString::fromLatin1(DefaultQueue)
                                                : cfg.printer;
    const QString owner = qEnvironmentVariable("USER");
    const QStringList command = choice.usb
        ? systemSetupCommandUsb(choice.uri, choice.model, queue, owner)
        : systemSetupCommand(choice.mac, choice.model, queue, owner);

    // Root steps go through pkexec in one go; without it, show the sudo command.
    const QString launcher = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (launcher.isEmpty()) {
        appendLog(tr("pkexec not found — please run this in a terminal:"));
        appendLog(tr("sudo ") + command.join(QLatin1Char(' ')));
        return;
    }

    setWorking(true, tr("System setup running (password prompt) …"));
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(launcher, command);
    process.waitForFinished(180000);
    const QString output = QString::fromLocal8Bit(process.readAll()).trimmed();
    if (!output.isEmpty())
        appendLog(output);

    if (process.exitCode() == 0) {
        cfg.printer = queue;
        cfg.model = choice.model;
        if (choice.usb) {
            cfg.device = choice.device;
        } else {
            cfg.mac = choice.mac;
            cfg.device = QStringLiteral("/dev/rfcomm0");
        }
        cfg.save();
        appendLog(tr("Setup complete."));
    } else if (process.exitCode() == 126 || process.exitCode() == 127) {
        appendLog(tr("Cancelled — no administrator rights granted."));
    } else {
        appendLog(tr("Setup failed (code %1).").arg(process.exitCode()));
    }
    setWorking(false);
    recheck();
}

} // namespace ptouch

#include "SetupDialog.moc"
