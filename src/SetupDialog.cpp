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

SetupDialog::SetupDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Set up the printer"));
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

    auto *deviceGroup = new QGroupBox(QStringLiteral("1. Find and pair the printer"));
    auto *deviceLayout = new QVBoxLayout(deviceGroup);
    auto *row = new QHBoxLayout;
    m_deviceBox = new QComboBox;
    m_deviceBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_deviceBox->setPlaceholderText(QStringLiteral("not searched yet"));
    m_scanButton = new QPushButton(QStringLiteral("Search"));
    m_pairButton = new QPushButton(QStringLiteral("Pair"));
    connect(m_scanButton, &QPushButton::clicked, this, &SetupDialog::scan);
    connect(m_pairButton, &QPushButton::clicked, this, &SetupDialog::pair);
    row->addWidget(m_deviceBox, 1);
    row->addWidget(m_scanButton);
    row->addWidget(m_pairButton);
    deviceLayout->addLayout(row);
    deviceLayout->addWidget(new QLabel(
        QStringLiteral("The printer has to be switched on. An existing connection to a "
                       "phone does not get in the way.")));
    v->addWidget(deviceGroup);

    auto *systemGroup = new QGroupBox(
        QStringLiteral("2. Set up the Bluetooth port and the print queue"));
    auto *systemLayout = new QVBoxLayout(systemGroup);
    systemLayout->addWidget(new QLabel(
        QStringLiteral("Creates the RFCOMM service, the access rule, the CUPS backend and "
                       "the print queue. This asks for the administrator password once.")));
    m_systemButton = new QPushButton(QStringLiteral("Set up now"));
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
    m_recheckButton = buttons->addButton(QStringLiteral("Check again"),
                                         QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    connect(m_recheckButton, &QPushButton::clicked, this, &SetupDialog::recheck);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    v->addWidget(buttons);

    recheck();
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
    QApplication::setOverrideCursor(working ? Qt::WaitCursor : Qt::ArrowCursor);
    if (!working)
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
        return ok ? QStringLiteral("<span style='color:#1e8449'>✓</span>")
                  : QStringLiteral("<span style='color:#c0392b'>✗</span>");
    };

    if (m_state.ready()) {
        m_summary->setText(QStringLiteral(
            "<h3 style='color:#1e8449'>Everything is set up</h3>"
            "<p>Queue <b>%1</b>, port <b>%2</b>.</p>").arg(m_state.queue, m_state.device));
    } else {
        m_summary->setText(QStringLiteral("<h3>Setup incomplete</h3>"));
    }

    QStringList lines;
    lines << QStringLiteral("%1 system tools (bluez, cups, ptouch driver)")
                 .arg(mark(m_state.dependenciesOk()));
    lines << QStringLiteral("%1 printer paired%2").arg(mark(m_state.paired),
              m_state.mac.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(m_state.mac));
    lines << QStringLiteral("%1 RFCOMM service running").arg(mark(m_state.rfcommActive));
    lines << QStringLiteral("%1 access to %2").arg(mark(m_state.deviceAccessible),
                                                   m_state.device);
    lines << QStringLiteral("%1 CUPS backend installed").arg(mark(m_state.cupsBackend));
    lines << QStringLiteral("%1 print queue %2").arg(mark(m_state.queuePresent),
              m_state.queue.isEmpty() ? QStringLiteral("—") : m_state.queue);

    const QStringList todo = m_state.missingSteps();
    if (!todo.isEmpty())
        lines << QStringLiteral("<br><b>Still to do:</b> %1").arg(todo.join(QStringLiteral(", ")));

    m_steps->setText(lines.join(QStringLiteral("<br>")));
    m_pairButton->setEnabled(m_deviceBox->count() > 0);
}

void SetupDialog::scan()
{
    setWorking(true, QStringLiteral("Searching for P-touch devices (about 12 seconds) …"));
    QApplication::processEvents();

    m_devices = scanForPrinters(12);
    m_deviceBox->clear();
    for (const Device &d : std::as_const(m_devices)) {
        m_deviceBox->addItem(QStringLiteral("%1 — %2").arg(d.name, d.mac), d.mac);
        appendLog(QStringLiteral("found: %1 (%2)").arg(d.name, d.mac));
    }
    if (m_devices.isEmpty())
        appendLog(QStringLiteral("No P-touch found. Switch the printer on and bring it "
                                 "into range."));
    setWorking(false);
    updateView();
}

void SetupDialog::pair()
{
    const int index = m_deviceBox->currentIndex();
    if (index < 0 || index >= m_devices.size())
        return;
    const Device device = m_devices.at(index);

    setWorking(true, QStringLiteral("Pairing with %1 …").arg(device.mac));
    QApplication::processEvents();

    QString output;
    const bool ok = pairDevice(device.mac, &output);
    if (!output.isEmpty())
        appendLog(output);
    appendLog(ok ? QStringLiteral("Pairing succeeded.") : QStringLiteral("Pairing failed."));

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
    QString mac = cfg.mac;
    QString model = cfg.model;
    if (m_deviceBox->currentIndex() >= 0 && m_deviceBox->currentIndex() < m_devices.size()) {
        mac = m_devices.at(m_deviceBox->currentIndex()).mac;
        model = m_devices.at(m_deviceBox->currentIndex()).model;
    }
    if (mac.isEmpty()) {
        appendLog(QStringLiteral("Search for a printer and pair it first."));
        return;
    }

    const QString queue = cfg.printer.isEmpty() ? QString::fromLatin1(DefaultQueue)
                                                : cfg.printer;
    const QString owner = qEnvironmentVariable("USER");
    const QStringList command = systemSetupCommand(mac, model, queue, owner);

    // Root steps go through pkexec in one go; without it, show the sudo command.
    const QString launcher = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (launcher.isEmpty()) {
        appendLog(QStringLiteral("pkexec not found — please run this in a terminal:"));
        appendLog(QStringLiteral("sudo ") + command.join(QLatin1Char(' ')));
        return;
    }

    setWorking(true, QStringLiteral("System setup running (password prompt) …"));
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(launcher, command);
    process.waitForFinished(180000);
    const QString output = QString::fromLocal8Bit(process.readAll()).trimmed();
    if (!output.isEmpty())
        appendLog(output);

    if (process.exitCode() == 0) {
        cfg.printer = queue;
        cfg.mac = mac;
        cfg.model = model;
        cfg.device = QStringLiteral("/dev/rfcomm0");
        cfg.save();
        appendLog(QStringLiteral("Setup complete."));
    } else if (process.exitCode() == 126 || process.exitCode() == 127) {
        appendLog(QStringLiteral("Cancelled — no administrator rights granted."));
    } else {
        appendLog(QStringLiteral("Setup failed (code %1).").arg(process.exitCode()));
    }
    setWorking(false);
    recheck();
}

} // namespace ptouch
