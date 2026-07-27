#include "MainWindow.h"

#include "PreviewWidget.h"
#include "Provision.h"
#include "SetupDialog.h"
#include "Version.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace ptouch {
namespace {
constexpr int PollIntervalMs = 20000;
constexpr int FreshnessMs = 10000;    // how long a tape reading counts as current
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral(PTOUCH_APP_NAME));
    resize(1040, 660);

    m_config = Config::load();
    m_job = new PrintJob(this);
    connect(m_job, &PrintJob::progress, this, [this](const QString &m) { setBusy(true, m); });
    connect(m_job, &PrintJob::finished, this, [this](bool ok, const QString &message) {
        setBusy(false);
        if (ok)
            statusBar()->showMessage(message, 6000);
        else
            QMessageBox::warning(this, QStringLiteral("Printing"), message);
        detectTape(true);
    });

    auto *central = new QWidget;
    auto *root = new QHBoxLayout(central);
    root->addWidget(buildControls(), 0);

    auto *right = new QVBoxLayout;
    m_preview = new PreviewWidget;
    right->addWidget(m_preview, 1);

    m_warning = new QLabel;
    m_warning->setWordWrap(true);
    m_warning->setStyleSheet(QStringLiteral("color: #c0392b;"));
    right->addWidget(m_warning);

    m_progress = new QProgressBar;
    m_progress->setRange(0, 0);            // indeterminate
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_progress->hide();
    right->addWidget(m_progress);

    auto *buttons = new QHBoxLayout;
    m_pdfButton = new QPushButton(QStringLiteral("Save as PDF …"));
    m_printButton = new QPushButton(QStringLiteral("Print"));
    m_printButton->setDefault(true);
    connect(m_pdfButton, &QPushButton::clicked, this, &MainWindow::savePdf);
    connect(m_printButton, &QPushButton::clicked, this, &MainWindow::doPrint);
    buttons->addStretch(1);
    buttons->addWidget(m_pdfButton);
    buttons->addWidget(m_printButton);
    right->addLayout(buttons);

    root->addLayout(right, 1);
    setCentralWidget(central);

    // Actions are created and connected separately: the addAction() overload
    // taking a functor only exists from Qt 6.3 onwards.
    auto *menu = menuBar()->addMenu(QStringLiteral("&Printer"));
    QAction *setupAction = menu->addAction(QStringLiteral("&Setup …"));
    connect(setupAction, &QAction::triggered, this, &MainWindow::openSetup);
    QAction *checkAction = menu->addAction(QStringLiteral("Check tape &now"));
    connect(checkAction, &QAction::triggered, this, [this] { detectTape(false); });
    menu->addSeparator();
    QAction *quitAction = menu->addAction(QStringLiteral("&Quit"));
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto *help = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction *aboutAction =
        help->addAction(QStringLiteral("About %1").arg(QStringLiteral(PTOUCH_APP_NAME)));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(this, QStringLiteral(PTOUCH_APP_NAME),
                           QStringLiteral("<h3>%1 %2</h3>"
                                          "<p>Labels for Brother P-touch tape cassettes.</p>"
                                          "<p><a href=\"%3\">%3</a></p>")
                               .arg(QStringLiteral(PTOUCH_APP_NAME),
                                    QStringLiteral(PTOUCH_VERSION),
                                    QStringLiteral(PTOUCH_HOMEPAGE)));
    });

    restoreSettings();
    m_building = false;
    m_detectedAt.start();
    refresh();

    // Keep an eye on which tape is loaded — only while the window is visible, so
    // the printer is not kept awake needlessly.
    m_poll = new QTimer(this);
    m_poll->setInterval(PollIntervalMs);
    connect(m_poll, &QTimer::timeout, this, [this] {
        if (isVisible() && !isMinimized())
            detectTape(true);
    });
    m_poll->start();

    QTimer::singleShot(0, this, [this] {
        const SetupState state = checkSetup();
        if (!state.ready()) {
            statusBar()->showMessage(QStringLiteral("Setup incomplete — see Printer → Setup"),
                                     12000);
            QTimer::singleShot(400, this, &MainWindow::openSetup);
        } else {
            detectTape(false);
        }
    });
}

MainWindow::~MainWindow()
{
    // The status worker talks to a Bluetooth port and can be mid-query when the
    // window closes. Letting it finish avoids tearing a running thread down.
    if (m_worker && m_worker->isRunning())
        m_worker->wait(5000);
}

void MainWindow::demoContent()
{
    m_demoMode = true;              // no printer chatter while posing for the camera
    m_poll->stop();
    m_text->setPlainText(QStringLiteral("⚡ Fuse box\nF3 · 16 A"));
    m_tape->setCurrentIndex(tapeIndex(12.0));
    m_copies->setValue(2);
    m_detected = 12.0;
    m_detectedAt.restart();
    m_status.mediaType = QStringLiteral("laminated");
    refresh();
    updateStatusLabel();
    statusBar()->showMessage(QStringLiteral("Ready"));
}

QWidget *MainWindow::buildControls()
{
    auto *box = new QWidget;
    box->setFixedWidth(350);
    auto *v = new QVBoxLayout(box);

    auto *tapeGroup = new QGroupBox(QStringLiteral("Tape"));
    auto *tapeForm = new QFormLayout(tapeGroup);
    m_tape = new QComboBox;
    for (const Tape &t : tapes())
        m_tape->addItem(QStringLiteral("%1 mm").arg(t.widthMm, 0, 'g', 3), t.widthMm);
    m_tape->setCurrentIndex(3);            // 12 mm
    connect(m_tape, &QComboBox::currentIndexChanged, this, &MainWindow::tapeChanged);
    tapeForm->addRow(QStringLiteral("Width"), m_tape);

    m_tapeStatus = new QLabel(QStringLiteral("looking for the printer …"));
    m_tapeStatus->setWordWrap(true);
    tapeForm->addRow(m_tapeStatus);

    m_adopt = new QPushButton;
    m_adopt->hide();
    connect(m_adopt, &QPushButton::clicked, this, &MainWindow::adoptDetectedTape);
    tapeForm->addRow(m_adopt);
    v->addWidget(tapeGroup);

    auto *textGroup = new QGroupBox(QStringLiteral("Text"));
    auto *textLayout = new QVBoxLayout(textGroup);
    m_text = new QPlainTextEdit(QStringLiteral("Example"));
    m_text->setPlaceholderText(QStringLiteral("Each line becomes a line on the label"));
    m_text->setFixedHeight(88);
    connect(m_text, &QPlainTextEdit::textChanged, this, &MainWindow::refresh);
    textLayout->addWidget(m_text);

    m_symbolButton = new QPushButton(QStringLiteral("Insert symbol …"));
    connect(m_symbolButton, &QPushButton::clicked, this, &MainWindow::insertSymbol);
    textLayout->addWidget(m_symbolButton);

    auto *form = new QFormLayout;
    m_font = new QFontComboBox;
    connect(m_font, &QFontComboBox::currentFontChanged, this, &MainWindow::refresh);
    form->addRow(QStringLiteral("Font"), m_font);

    auto *style = new QHBoxLayout;
    m_bold = new QCheckBox(QStringLiteral("Bold"));
    m_bold->setChecked(true);
    m_italic = new QCheckBox(QStringLiteral("Italic"));
    for (QCheckBox *c : {m_bold, m_italic}) {
        connect(c, &QCheckBox::toggled, this, &MainWindow::refresh);
        style->addWidget(c);
    }
    style->addStretch(1);
    form->addRow(QStringLiteral("Style"), style);

    auto *sizeRow = new QHBoxLayout;
    m_autoSize = new QCheckBox(QStringLiteral("automatic"));
    m_autoSize->setChecked(true);
    m_size = new QDoubleSpinBox;
    m_size->setRange(4, 200);
    m_size->setValue(20);
    m_size->setSuffix(QStringLiteral(" pt"));
    connect(m_autoSize, &QCheckBox::toggled, this, &MainWindow::refresh);
    connect(m_size, &QDoubleSpinBox::valueChanged, this, &MainWindow::refresh);
    sizeRow->addWidget(m_autoSize);
    sizeRow->addWidget(m_size);
    form->addRow(QStringLiteral("Size"), sizeRow);

    m_align = new QComboBox;
    m_align->addItem(QStringLiteral("Left"), int(Align::Left));
    m_align->addItem(QStringLiteral("Centre"), int(Align::Center));
    m_align->addItem(QStringLiteral("Right"), int(Align::Right));
    m_align->setCurrentIndex(1);
    connect(m_align, &QComboBox::currentIndexChanged, this, &MainWindow::refresh);
    form->addRow(QStringLiteral("Alignment"), m_align);

    m_lineSpacing = new QDoubleSpinBox;
    m_lineSpacing->setRange(0.6, 3.0);
    m_lineSpacing->setSingleStep(0.05);
    m_lineSpacing->setValue(1.1);
    connect(m_lineSpacing, &QDoubleSpinBox::valueChanged, this, &MainWindow::refresh);
    form->addRow(QStringLiteral("Line spacing"), m_lineSpacing);
    textLayout->addLayout(form);
    v->addWidget(textGroup);

    auto *labelGroup = new QGroupBox(QStringLiteral("Label"));
    auto *labelForm = new QFormLayout(labelGroup);

    auto *lengthRow = new QHBoxLayout;
    m_autoLength = new QCheckBox(QStringLiteral("automatic"));
    m_autoLength->setChecked(true);
    m_length = new QDoubleSpinBox;
    m_length->setRange(8, 500);
    m_length->setValue(50);
    m_length->setSuffix(QStringLiteral(" mm"));
    connect(m_autoLength, &QCheckBox::toggled, this, &MainWindow::refresh);
    connect(m_length, &QDoubleSpinBox::valueChanged, this, &MainWindow::refresh);
    lengthRow->addWidget(m_autoLength);
    lengthRow->addWidget(m_length);
    labelForm->addRow(QStringLiteral("Length"), lengthRow);

    m_margin = new QDoubleSpinBox;
    m_margin->setRange(0, 40);
    m_margin->setValue(3);
    m_margin->setSuffix(QStringLiteral(" mm"));
    connect(m_margin, &QDoubleSpinBox::valueChanged, this, &MainWindow::refresh);
    labelForm->addRow(QStringLiteral("Side margin"), m_margin);

    m_frame = new QCheckBox(QStringLiteral("Frame"));
    m_mirror = new QCheckBox(QStringLiteral("Mirror (tape read from behind)"));
    m_cut = new QCheckBox(QStringLiteral("Cut automatically"));
    m_cut->setChecked(true);
    for (QCheckBox *c : {m_frame, m_mirror, m_cut}) {
        connect(c, &QCheckBox::toggled, this, &MainWindow::refresh);
        labelForm->addRow(c);
    }

    m_copies = new QSpinBox;
    m_copies->setRange(1, 99);
    connect(m_copies, &QSpinBox::valueChanged, this, &MainWindow::refresh);
    labelForm->addRow(QStringLiteral("Copies"), m_copies);
    v->addWidget(labelGroup);

    v->addStretch(1);
    return box;
}

Spec MainWindow::collectSpec() const
{
    Spec s;
    s.text = m_text->toPlainText().isEmpty() ? QStringLiteral(" ") : m_text->toPlainText();
    s.tapeMm = m_tape->currentData().toDouble();
    s.family = m_font->currentFont().family();
    s.bold = m_bold->isChecked();
    s.italic = m_italic->isChecked();
    s.autoSize = m_autoSize->isChecked();
    s.sizePt = m_size->value();
    s.align = Align(m_align->currentData().toInt());
    s.autoLength = m_autoLength->isChecked();
    s.lengthMm = m_length->value();
    s.marginMm = m_margin->value();
    s.lineSpacing = m_lineSpacing->value();
    s.frame = m_frame->isChecked();
    s.mirror = m_mirror->isChecked();
    s.autocut = m_cut->isChecked();
    s.copies = m_copies->value();
    return s;
}

void MainWindow::refresh()
{
    if (m_building)
        return;
    m_spec = collectSpec();
    m_size->setEnabled(!m_spec.autoSize);
    m_length->setEnabled(!m_spec.autoLength);
    m_layout = computeLayout(m_spec);
    m_warning->setText(m_layout.warnings.join(QStringLiteral("  ·  ")));
    m_preview->setLabel(m_spec, m_layout);
    storeSettings();
}

void MainWindow::tapeChanged()
{
    if (!m_settingTape) {
        // Picking the loaded width again also switches automatic tracking back on.
        m_manualTape = !qFuzzyCompare(m_tape->currentData().toDouble(), m_detected);
    }
    refresh();
    updateStatusLabel();
}

void MainWindow::adoptDetectedTape()
{
    if (tapeIndex(m_detected) < 0)
        return;
    m_manualTape = false;
    m_settingTape = true;
    m_tape->setCurrentIndex(tapeIndex(m_detected));
    m_settingTape = false;
    refresh();
    updateStatusLabel();
}

void MainWindow::detectTape(bool quiet)
{
    if (m_demoMode || (m_worker && m_worker->isRunning()))
        return;
    if (!quiet) {
        m_tapeStatus->setText(QStringLiteral("asking the printer …"));
        m_tapeStatus->setStyleSheet(QString());
    }
    m_worker = new StatusWorker(m_config.device, this);
    connect(m_worker, &StatusWorker::finished, this, &MainWindow::statusArrived);
    connect(m_worker, &StatusWorker::finished, m_worker, &QObject::deleteLater);
    m_worker->start();
}

void MainWindow::statusArrived(const Status &status)
{
    m_status = status;
    if (status.error.isEmpty() && status.tapeMm > 0) {
        m_detected = status.tapeMm;
        m_detectedAt.restart();
        if (!m_manualTape && tapeIndex(m_detected) >= 0
            && !qFuzzyCompare(m_tape->currentData().toDouble(), m_detected)) {
            m_settingTape = true;
            m_tape->setCurrentIndex(tapeIndex(m_detected));   // follow along quietly
            m_settingTape = false;
            refresh();
        }
    } else {
        m_detected = 0;
    }
    updateStatusLabel();

    if (m_printPending) {                  // a print is waiting for this check
        m_printPending = false;
        confirmAndPrint();
    }
}

void MainWindow::updateStatusLabel()
{
    const double selected = m_tape->currentData().toDouble();
    const bool mismatch = m_detected > 0 && !qFuzzyCompare(selected, m_detected);

    m_adopt->setVisible(mismatch && tapeIndex(m_detected) >= 0);
    if (mismatch) {
        m_adopt->setText(QStringLiteral("switch to %1 mm").arg(m_detected, 0, 'g', 3));
        m_tapeStatus->setText(QStringLiteral("⚠ loaded: %1 mm — selected: %2 mm")
                                  .arg(m_detected, 0, 'g', 3).arg(selected, 0, 'g', 3));
        m_tapeStatus->setStyleSheet(QStringLiteral("color: #b9770e;"));
    } else if (m_detected > 0) {
        m_tapeStatus->setText(QStringLiteral("✓ %1 mm, %2, ready")
                                  .arg(m_detected, 0, 'g', 3).arg(m_status.mediaType));
        m_tapeStatus->setStyleSheet(QStringLiteral("color: #1e8449;"));
    } else if (!m_status.error.isEmpty() || !m_status.errors.isEmpty()) {
        const QString message = m_status.errors.isEmpty()
                                    ? m_status.error
                                    : m_status.errors.join(QStringLiteral(", "));
        m_tapeStatus->setText(QStringLiteral("✗ %1").arg(message));
        m_tapeStatus->setStyleSheet(QStringLiteral("color: #c0392b;"));
    }
}

void MainWindow::setBusy(bool busy, const QString &message)
{
    m_printButton->setEnabled(!busy);
    m_pdfButton->setEnabled(!busy);
    m_progress->setVisible(busy);
    m_printButton->setText(busy ? QStringLiteral("Printing …") : QStringLiteral("Print"));
    if (busy && !message.isEmpty())
        statusBar()->showMessage(message);
    else if (!busy)
        statusBar()->clearMessage();
}

void MainWindow::doPrint()
{
    if (m_job->running())
        return;
    if (m_layout.overflow
        && QMessageBox::question(this, QStringLiteral("Text too long"),
                                 QStringLiteral("The text does not fit the chosen length "
                                                "and will be cut off.\nPrint anyway?"))
               != QMessageBox::Yes)
        return;

    // Make sure the tape reading is fresh before committing material.
    if (!m_detectedAt.isValid() || m_detectedAt.elapsed() > FreshnessMs) {
        m_printPending = true;
        setBusy(true, QStringLiteral("checking the loaded tape …"));
        detectTape(true);
        return;
    }
    confirmAndPrint();
}

void MainWindow::confirmAndPrint()
{
    setBusy(false);
    const double selected = m_spec.tapeMm;

    if (m_detected > 0 && !qFuzzyCompare(selected, m_detected)) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("Wrong tape width"));
        box.setText(QStringLiteral("The printer holds %1 mm tape, the label is laid out "
                                   "for %2 mm.")
                        .arg(m_detected, 0, 'g', 3).arg(selected, 0, 'g', 3));
        box.setInformativeText(QStringLiteral("Printers usually refuse labels whose tape "
                                              "width does not match."));
        QPushButton *adopt = box.addButton(
            QStringLiteral("Switch to %1 mm").arg(m_detected, 0, 'g', 3),
            QMessageBox::AcceptRole);
        QPushButton *anyway = box.addButton(QStringLiteral("Print anyway"),
                                            QMessageBox::DestructiveRole);
        box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
        box.setDefaultButton(adopt);
        box.exec();
        if (box.clickedButton() == adopt)
            adoptDetectedTape();
        else if (box.clickedButton() != anyway)
            return;
    } else if (m_detected <= 0 && (!m_status.error.isEmpty() || !m_status.errors.isEmpty())) {
        const QString message = m_status.errors.isEmpty()
                                    ? m_status.error
                                    : m_status.errors.join(QStringLiteral(", "));
        if (QMessageBox::question(this, QStringLiteral("Printer not ready"),
                                  QStringLiteral("%1\n\nSubmit the job anyway? It will be "
                                                 "printed as soon as the printer is "
                                                 "reachable again.").arg(message),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;
    }

    setBusy(true, QStringLiteral("Submitting job …"));
    m_job->start(m_spec, m_layout, m_config.printer);
}

void MainWindow::insertSymbol()
{
    // Outline symbols first: those print as crisp vectors. The pictographs below
    // come from a colour font and take the image path, which is coarser on tape.
    static const QStringList outlines = {
        QStringLiteral("→"), QStringLiteral("←"), QStringLiteral("↑"), QStringLiteral("↓"),
        QStringLiteral("★"), QStringLiteral("☆"), QStringLiteral("●"), QStringLiteral("■"),
        QStringLiteral("✓"), QStringLiteral("✗"), QStringLiteral("⚠"), QStringLiteral("⚡"),
        QStringLiteral("☎"), QStringLiteral("✉"), QStringLiteral("♻"), QStringLiteral("☣"),
        QStringLiteral("°"), QStringLiteral("±"), QStringLiteral("µ"), QStringLiteral("Ω"),
        QStringLiteral("€"), QStringLiteral("§"), QStringLiteral("№"), QStringLiteral("⌀"),
    };
    static const QStringList pictographs = {
        QStringLiteral("🔧"), QStringLiteral("🔌"), QStringLiteral("💡"), QStringLiteral("🔋"),
        QStringLiteral("📦"), QStringLiteral("🗄"), QStringLiteral("🔑"), QStringLiteral("🧰"),
        QStringLiteral("🧪"), QStringLiteral("🌡"), QStringLiteral("❄"), QStringLiteral("🔥"),
    };

    QMenu menu(this);
    const auto addRow = [&menu, this](const QStringList &items) {
        for (const QString &symbol : items) {
            QAction *action = menu.addAction(symbol);
            connect(action, &QAction::triggered, this, [this, symbol] {
                m_text->insertPlainText(symbol);
                m_text->setFocus();
            });
        }
    };
    addRow(outlines);
    menu.addSeparator();
    QAction *note = menu.addAction(QStringLiteral("— printed as an image —"));
    note->setEnabled(false);
    addRow(pictographs);

    QFont big = menu.font();
    big.setPointSizeF(big.pointSizeF() * 1.4);
    menu.setFont(big);
    menu.exec(m_symbolButton->mapToGlobal(QPoint(0, m_symbolButton->height())));
}

void MainWindow::savePdf()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save label"),
        QDir::homePath() + QStringLiteral("/label.pdf"), QStringLiteral("PDF (*.pdf)"));
    if (path.isEmpty())
        return;
    writePdf(path, m_spec, m_layout);
    statusBar()->showMessage(QStringLiteral("saved: %1").arg(path), 6000);
}

void MainWindow::openSetup()
{
    SetupDialog dialog(this);
    dialog.exec();
    m_config = Config::load();
    detectTape(false);
}

void MainWindow::changeEvent(QEvent *event)
{
    // Check again when the window is activated — the tape may have been swapped.
    if (event->type() == QEvent::ActivationChange && isActiveWindow()
        && m_detectedAt.isValid() && m_detectedAt.elapsed() > 5000) {
        detectTape(true);
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::storeSettings() const
{
    QSettings s;
    s.setValue(QStringLiteral("font"), m_font->currentFont().family());
    s.setValue(QStringLiteral("bold"), m_bold->isChecked());
    s.setValue(QStringLiteral("italic"), m_italic->isChecked());
    s.setValue(QStringLiteral("autoSize"), m_autoSize->isChecked());
    s.setValue(QStringLiteral("size"), m_size->value());
    s.setValue(QStringLiteral("align"), m_align->currentIndex());
    s.setValue(QStringLiteral("autoLength"), m_autoLength->isChecked());
    s.setValue(QStringLiteral("length"), m_length->value());
    s.setValue(QStringLiteral("margin"), m_margin->value());
    s.setValue(QStringLiteral("lineSpacing"), m_lineSpacing->value());
    s.setValue(QStringLiteral("frame"), m_frame->isChecked());
    s.setValue(QStringLiteral("cut"), m_cut->isChecked());
}

void MainWindow::restoreSettings()
{
    QSettings s;
    if (s.contains(QStringLiteral("font")))
        m_font->setCurrentFont(QFont(s.value(QStringLiteral("font")).toString()));
    else
        m_font->setCurrentFont(QFont(QStringLiteral("DejaVu Sans")));
    m_bold->setChecked(s.value(QStringLiteral("bold"), true).toBool());
    m_italic->setChecked(s.value(QStringLiteral("italic"), false).toBool());
    m_autoSize->setChecked(s.value(QStringLiteral("autoSize"), true).toBool());
    m_size->setValue(s.value(QStringLiteral("size"), 20.0).toDouble());
    m_align->setCurrentIndex(s.value(QStringLiteral("align"), 1).toInt());
    m_autoLength->setChecked(s.value(QStringLiteral("autoLength"), true).toBool());
    m_length->setValue(s.value(QStringLiteral("length"), 50.0).toDouble());
    m_margin->setValue(s.value(QStringLiteral("margin"), 3.0).toDouble());
    m_lineSpacing->setValue(s.value(QStringLiteral("lineSpacing"), 1.1).toDouble());
    m_frame->setChecked(s.value(QStringLiteral("frame"), false).toBool());
    m_cut->setChecked(s.value(QStringLiteral("cut"), true).toBool());
}

} // namespace ptouch
