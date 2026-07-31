#include "MainWindow.h"

#include "PreviewWidget.h"
#include "Provision.h"
#include "SetupDialog.h"
#include "Version.h"

#include <QAction>
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
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
            QMessageBox::warning(this, tr("Printing"), message);
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
    m_pdfButton = new QPushButton(tr("Save as PDF …"));
    m_printButton = new QPushButton(tr("Print"));
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
    auto *menu = menuBar()->addMenu(tr("&Printer"));
    QAction *setupAction = menu->addAction(tr("&Setup …"));
    connect(setupAction, &QAction::triggered, this, &MainWindow::openSetup);
    QAction *checkAction = menu->addAction(tr("Check tape &now"));
    connect(checkAction, &QAction::triggered, this, [this] { detectTape(false); });
    menu->addSeparator();
    QAction *quitAction = menu->addAction(tr("&Quit"));
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    m_templateMenu = menuBar()->addMenu(tr("&Templates"));
    QAction *saveTpl = m_templateMenu->addAction(tr("&Save current settings …"));
    connect(saveTpl, &QAction::triggered, this, &MainWindow::saveTemplate);
    QAction *manageTpl = m_templateMenu->addAction(tr("&Delete a template …"));
    connect(manageTpl, &QAction::triggered, this, &MainWindow::manageTemplates);
    m_templateMenu->addSeparator();
    connect(m_templateMenu, &QMenu::aboutToShow, this, [this] {
        // Rebuild the list each time — templates may have been added since.
        const QList<QAction *> actions = m_templateMenu->actions();
        for (int i = 3; i < actions.size(); ++i)
            m_templateMenu->removeAction(actions.at(i));
        QSettings s;
        s.beginGroup(QStringLiteral("templates"));
        for (const QString &name : s.childGroups()) {
            QAction *a = m_templateMenu->addAction(name);
            a->setData(name);
            connect(a, &QAction::triggered, this, &MainWindow::loadTemplate);
        }
    });

    auto *help = menuBar()->addMenu(tr("&Help"));
    QAction *aboutAction =
        help->addAction(tr("About %1").arg(QStringLiteral(PTOUCH_APP_NAME)));
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
            statusBar()->showMessage(tr("Setup incomplete — see Printer → Setup"),
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
    m_text->setPlainText(tr("⚡ Fuse box\nF3 · 16 A"));
    m_tape->setCurrentIndex(tapeIndex(12.0));
    m_copies->setValue(2);
    if (qrAvailable()) {
        m_artworkKind->setCurrentIndex(m_artworkKind->findData(int(CodeType::Qr)));
        m_artworkData->setText(QStringLiteral("https://github.com/tombueng/ptouch-studio"));
    }
    m_detected = 12.0;
    m_detectedAt.restart();
    m_status.mediaType = QCoreApplication::translate("Printer", "laminated");
    refresh();
    updateStatusLabel();
    statusBar()->showMessage(tr("Ready"));
}

QWidget *MainWindow::buildControls()
{
    auto *box = new QWidget;
    box->setFixedWidth(350);
    auto *v = new QVBoxLayout(box);

    auto *tapeGroup = new QGroupBox(tr("Tape"));
    auto *tapeForm = new QFormLayout(tapeGroup);
    m_tape = new QComboBox;
    for (const Tape &t : tapes())
        m_tape->addItem(tr("%1 mm").arg(t.widthMm, 0, 'g', 3), t.widthMm);
    m_tape->setCurrentIndex(3);            // 12 mm
    connect(m_tape, &QComboBox::currentIndexChanged, this, &MainWindow::tapeChanged);
    tapeForm->addRow(tr("Width"), m_tape);

    m_tapeStatus = new QLabel(tr("looking for the printer …"));
    m_tapeStatus->setWordWrap(true);
    tapeForm->addRow(m_tapeStatus);

    m_adopt = new QPushButton;
    m_adopt->hide();
    connect(m_adopt, &QPushButton::clicked, this, &MainWindow::adoptDetectedTape);
    tapeForm->addRow(m_adopt);
    v->addWidget(tapeGroup);

    auto *textGroup = new QGroupBox(tr("Text"));
    auto *textLayout = new QVBoxLayout(textGroup);
    m_text = new QPlainTextEdit(tr("Example"));
    m_text->setPlaceholderText(tr("Each line becomes a line on the label"));
    m_text->setFixedHeight(88);
    connect(m_text, &QPlainTextEdit::textChanged, this, &MainWindow::refresh);
    textLayout->addWidget(m_text);

    m_symbolButton = new QPushButton(tr("Insert symbol …"));
    connect(m_symbolButton, &QPushButton::clicked, this, &MainWindow::insertSymbol);
    textLayout->addWidget(m_symbolButton);

    auto *form = new QFormLayout;
    m_font = new QFontComboBox;
    connect(m_font, &QFontComboBox::currentFontChanged, this, &MainWindow::refresh);
    form->addRow(tr("Font"), m_font);

    auto *style = new QHBoxLayout;
    m_bold = new QCheckBox(tr("Bold"));
    m_bold->setChecked(true);
    m_italic = new QCheckBox(tr("Italic"));
    for (QCheckBox *c : {m_bold, m_italic}) {
        connect(c, &QCheckBox::toggled, this, &MainWindow::refresh);
        style->addWidget(c);
    }
    style->addStretch(1);
    form->addRow(tr("Style"), style);

    auto *sizeRow = new QHBoxLayout;
    m_autoSize = new QCheckBox(tr("automatic"));
    m_autoSize->setChecked(true);
    m_size = new QDoubleSpinBox;
    m_size->setRange(4, 200);
    m_size->setValue(20);
    m_size->setSuffix(tr(" pt"));
    connect(m_autoSize, &QCheckBox::toggled, this, &MainWindow::refresh);
    connect(m_size, &QDoubleSpinBox::valueChanged, this, &MainWindow::refresh);
    sizeRow->addWidget(m_autoSize);
    sizeRow->addWidget(m_size);
    form->addRow(tr("Size"), sizeRow);

    m_align = new QComboBox;
    m_align->addItem(tr("Left"), int(Align::Left));
    m_align->addItem(tr("Centre"), int(Align::Center));
    m_align->addItem(tr("Right"), int(Align::Right));
    m_align->setCurrentIndex(1);
    connect(m_align, &QComboBox::currentIndexChanged, this, &MainWindow::refresh);
    form->addRow(tr("Alignment"), m_align);

    m_lineSpacing = new QDoubleSpinBox;
    m_lineSpacing->setRange(0.6, 3.0);
    m_lineSpacing->setSingleStep(0.05);
    m_lineSpacing->setValue(1.1);
    connect(m_lineSpacing, &QDoubleSpinBox::valueChanged, this, &MainWindow::refresh);
    form->addRow(tr("Line spacing"), m_lineSpacing);
    textLayout->addLayout(form);
    v->addWidget(textGroup);

    auto *artGroup = new QGroupBox(tr("Picture or code"));
    auto *artForm = new QFormLayout(artGroup);

    m_artworkKind = new QComboBox;
    m_artworkKind->addItem(tr("none"), int(CodeType::None));
    m_artworkKind->addItem(tr("Picture …"), -1);          // -1 marks the file case
    m_artworkKind->addItem(tr("QR code"), int(CodeType::Qr));
    m_artworkKind->addItem(tr("Barcode (Code 128)"), int(CodeType::Code128));
    if (!qrAvailable())
        m_artworkKind->removeItem(2);   // built without libqrencode
    connect(m_artworkKind, &QComboBox::currentIndexChanged,
            this, &MainWindow::artworkKindChanged);
    artForm->addRow(tr("Kind"), m_artworkKind);

    auto *artRow = new QHBoxLayout;
    m_artworkData = new QLineEdit;
    m_artworkData->setPlaceholderText(tr("content of the code"));
    m_artworkBrowse = new QPushButton(tr("Choose …"));
    connect(m_artworkData, &QLineEdit::textChanged, this, &MainWindow::refresh);
    connect(m_artworkBrowse, &QPushButton::clicked, this, &MainWindow::chooseArtwork);
    artRow->addWidget(m_artworkData, 1);
    artRow->addWidget(m_artworkBrowse);
    artForm->addRow(tr("Content"), artRow);

    m_artworkSide = new QComboBox;
    m_artworkSide->addItem(tr("Left"), int(Spec::Side::Left));
    m_artworkSide->addItem(tr("Right"), int(Spec::Side::Right));
    connect(m_artworkSide, &QComboBox::currentIndexChanged, this, &MainWindow::refresh);
    artForm->addRow(tr("Side"), m_artworkSide);
    v->addWidget(artGroup);

    auto *labelGroup = new QGroupBox(tr("Label"));
    auto *labelForm = new QFormLayout(labelGroup);

    auto *lengthRow = new QHBoxLayout;
    m_autoLength = new QCheckBox(tr("automatic"));
    m_autoLength->setChecked(true);
    m_length = new QDoubleSpinBox;
    m_length->setRange(8, 500);
    m_length->setValue(50);
    m_length->setSuffix(tr(" mm"));
    connect(m_autoLength, &QCheckBox::toggled, this, &MainWindow::refresh);
    connect(m_length, &QDoubleSpinBox::valueChanged, this, &MainWindow::refresh);
    lengthRow->addWidget(m_autoLength);
    lengthRow->addWidget(m_length);
    labelForm->addRow(tr("Length"), lengthRow);

    m_margin = new QDoubleSpinBox;
    m_margin->setRange(0, 40);
    m_margin->setValue(3);
    m_margin->setSuffix(tr(" mm"));
    connect(m_margin, &QDoubleSpinBox::valueChanged, this, &MainWindow::refresh);
    labelForm->addRow(tr("Side margin"), m_margin);

    m_frame = new QCheckBox(tr("Frame"));
    m_mirror = new QCheckBox(tr("Mirror (tape read from behind)"));
    m_cut = new QCheckBox(tr("Cut automatically"));
    m_cut->setChecked(true);
    for (QCheckBox *c : {m_frame, m_mirror, m_cut}) {
        connect(c, &QCheckBox::toggled, this, &MainWindow::refresh);
        labelForm->addRow(c);
    }

    m_copies = new QSpinBox;
    m_copies->setRange(1, 99);
    connect(m_copies, &QSpinBox::valueChanged, this, &MainWindow::refresh);
    labelForm->addRow(tr("Copies"), m_copies);
    v->addWidget(labelGroup);

    v->addStretch(1);
    return box;
}

Spec MainWindow::collectSpec() const
{
    Spec s;
    s.text = m_text->toPlainText().isEmpty() ? tr(" ") : m_text->toPlainText();
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
    s.model = m_config.model;   // decides how much of the tape the head reaches

    const int kind = m_artworkKind->currentData().toInt();
    if (kind == -1)
        s.picturePath = m_artworkData->text();
    else if (kind != int(CodeType::None)) {
        s.codeType = CodeType(kind);
        s.codeData = m_artworkData->text();
    }
    s.artworkSide = Spec::Side(m_artworkSide->currentData().toInt());
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
    m_warning->setText(m_layout.warnings.join(tr("  ·  ")));
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
        m_tapeStatus->setText(tr("asking the printer …"));
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
        m_adopt->setText(tr("switch to %1 mm").arg(m_detected, 0, 'g', 3));
        m_tapeStatus->setText(tr("⚠ loaded: %1 mm — selected: %2 mm")
                                  .arg(m_detected, 0, 'g', 3).arg(selected, 0, 'g', 3));
        m_tapeStatus->setStyleSheet(QStringLiteral("color: #b9770e;"));
    } else if (m_detected > 0) {
        m_tapeStatus->setText(tr("✓ %1 mm, %2, ready")
                                  .arg(m_detected, 0, 'g', 3).arg(m_status.mediaType));
        m_tapeStatus->setStyleSheet(QStringLiteral("color: #1e8449;"));
    } else if (!m_status.error.isEmpty() || !m_status.errors.isEmpty()) {
        const QString message = m_status.errors.isEmpty()
                                    ? m_status.error
                                    : m_status.errors.join(tr(", "));
        // A reported error sticks until the printer loses power — saying so
        // saves people from hunting for a reset that does not exist.
        m_tapeStatus->setText(m_status.reportsError()
            ? tr("✗ %1 — switch the printer off and on again").arg(message)
            : tr("✗ %1").arg(message));
        m_tapeStatus->setStyleSheet(QStringLiteral("color: #c0392b;"));
    }
}

void MainWindow::setBusy(bool busy, const QString &message)
{
    m_printButton->setEnabled(!busy);
    m_pdfButton->setEnabled(!busy);
    m_progress->setVisible(busy);
    m_printButton->setText(busy ? tr("Printing …") : tr("Print"));
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
        && QMessageBox::question(this, tr("Text too long"),
                                 QStringLiteral("The text does not fit the chosen length "
                                                "and will be cut off.\nPrint anyway?"))
               != QMessageBox::Yes)
        return;

    // Make sure the tape reading is fresh before committing material.
    if (!m_detectedAt.isValid() || m_detectedAt.elapsed() > FreshnessMs) {
        m_printPending = true;
        setBusy(true, tr("checking the loaded tape …"));
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
        box.setWindowTitle(tr("Wrong tape width"));
        box.setText(QStringLiteral("The printer holds %1 mm tape, the label is laid out "
                                   "for %2 mm.")
                        .arg(m_detected, 0, 'g', 3).arg(selected, 0, 'g', 3));
        box.setInformativeText(QStringLiteral("Printers usually refuse labels whose tape "
                                              "width does not match."));
        QPushButton *adopt = box.addButton(
            tr("Switch to %1 mm").arg(m_detected, 0, 'g', 3),
            QMessageBox::AcceptRole);
        QPushButton *anyway = box.addButton(tr("Print anyway"),
                                            QMessageBox::DestructiveRole);
        box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.setDefaultButton(adopt);
        box.exec();
        if (box.clickedButton() == adopt)
            adoptDetectedTape();
        else if (box.clickedButton() != anyway)
            return;
    } else if (m_detected <= 0 && (!m_status.error.isEmpty() || !m_status.errors.isEmpty())) {
        const QString message = m_status.errors.isEmpty()
                                    ? m_status.error
                                    : m_status.errors.join(tr(", "));
        if (QMessageBox::question(this, tr("Printer not ready"),
                                  QStringLiteral("%1\n\nSubmit the job anyway? It will be "
                                                 "printed as soon as the printer is "
                                                 "reachable again.").arg(message),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;
    }

    setBusy(true, tr("Submitting job …"));
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
    QAction *note = menu.addAction(tr("— printed as an image —"));
    note->setEnabled(false);
    addRow(pictographs);

    QFont big = menu.font();
    big.setPointSizeF(big.pointSizeF() * 1.4);
    menu.setFont(big);
    menu.exec(m_symbolButton->mapToGlobal(QPoint(0, m_symbolButton->height())));
}

void MainWindow::saveTemplate()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save template"),
                                               tr("Name:"), QLineEdit::Normal,
                                               QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;

    const Spec s = collectSpec();
    QSettings settings;
    settings.beginGroup(QStringLiteral("templates"));
    settings.beginGroup(name);
    settings.setValue(QStringLiteral("tapeMm"), s.tapeMm);
    settings.setValue(QStringLiteral("family"), s.family);
    settings.setValue(QStringLiteral("bold"), s.bold);
    settings.setValue(QStringLiteral("italic"), s.italic);
    settings.setValue(QStringLiteral("autoSize"), s.autoSize);
    settings.setValue(QStringLiteral("sizePt"), s.sizePt);
    settings.setValue(QStringLiteral("align"), int(s.align));
    settings.setValue(QStringLiteral("autoLength"), s.autoLength);
    settings.setValue(QStringLiteral("lengthMm"), s.lengthMm);
    settings.setValue(QStringLiteral("marginMm"), s.marginMm);
    settings.setValue(QStringLiteral("lineSpacing"), s.lineSpacing);
    settings.setValue(QStringLiteral("frame"), s.frame);
    settings.setValue(QStringLiteral("mirror"), s.mirror);
    settings.setValue(QStringLiteral("copies"), s.copies);
    settings.setValue(QStringLiteral("codeType"), int(s.codeType));
    settings.setValue(QStringLiteral("artworkSide"), int(s.artworkSide));
    statusBar()->showMessage(tr("Template \"%1\" saved.").arg(name), 5000);
}

void MainWindow::loadTemplate()
{
    auto *action = qobject_cast<QAction *>(sender());
    if (!action)
        return;
    const QString name = action->data().toString();

    QSettings settings;
    settings.beginGroup(QStringLiteral("templates"));
    settings.beginGroup(name);

    Spec s = collectSpec();
    s.tapeMm = settings.value(QStringLiteral("tapeMm"), s.tapeMm).toDouble();
    s.family = settings.value(QStringLiteral("family"), s.family).toString();
    s.bold = settings.value(QStringLiteral("bold"), s.bold).toBool();
    s.italic = settings.value(QStringLiteral("italic"), s.italic).toBool();
    s.autoSize = settings.value(QStringLiteral("autoSize"), s.autoSize).toBool();
    s.sizePt = settings.value(QStringLiteral("sizePt"), s.sizePt).toDouble();
    s.align = Align(settings.value(QStringLiteral("align"), int(s.align)).toInt());
    s.autoLength = settings.value(QStringLiteral("autoLength"), s.autoLength).toBool();
    s.lengthMm = settings.value(QStringLiteral("lengthMm"), s.lengthMm).toDouble();
    s.marginMm = settings.value(QStringLiteral("marginMm"), s.marginMm).toDouble();
    s.lineSpacing = settings.value(QStringLiteral("lineSpacing"), s.lineSpacing).toDouble();
    s.frame = settings.value(QStringLiteral("frame"), s.frame).toBool();
    s.mirror = settings.value(QStringLiteral("mirror"), s.mirror).toBool();
    s.copies = settings.value(QStringLiteral("copies"), s.copies).toInt();
    s.codeType = CodeType(settings.value(QStringLiteral("codeType"),
                                         int(s.codeType)).toInt());
    s.artworkSide = Spec::Side(settings.value(QStringLiteral("artworkSide"),
                                              int(s.artworkSide)).toInt());
    applySpec(s);
    statusBar()->showMessage(tr("Template \"%1\" loaded.").arg(name), 5000);
}

void MainWindow::manageTemplates()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("templates"));
    const QStringList names = settings.childGroups();
    if (names.isEmpty()) {
        QMessageBox::information(this, tr("Templates"), tr("No templates saved yet."));
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getItem(this, tr("Delete a template"),
                                               tr("Template:"), names, 0, false, &ok);
    if (ok && !name.isEmpty()) {
        settings.remove(name);
        statusBar()->showMessage(tr("Template \"%1\" deleted.").arg(name), 5000);
    }
}

void MainWindow::applySpec(const Spec &spec)
{
    m_building = true;                 // one refresh at the end, not per widget
    if (tapeIndex(spec.tapeMm) >= 0) {
        m_manualTape = true;
        m_settingTape = true;
        m_tape->setCurrentIndex(tapeIndex(spec.tapeMm));
        m_settingTape = false;
    }
    m_font->setCurrentFont(QFont(spec.family));
    m_bold->setChecked(spec.bold);
    m_italic->setChecked(spec.italic);
    m_autoSize->setChecked(spec.autoSize);
    m_size->setValue(spec.sizePt);
    m_align->setCurrentIndex(m_align->findData(int(spec.align)));
    m_autoLength->setChecked(spec.autoLength);
    m_length->setValue(spec.lengthMm);
    m_margin->setValue(spec.marginMm);
    m_lineSpacing->setValue(spec.lineSpacing);
    m_frame->setChecked(spec.frame);
    m_mirror->setChecked(spec.mirror);
    m_copies->setValue(spec.copies);
    m_artworkKind->setCurrentIndex(std::max(0, m_artworkKind->findData(int(spec.codeType))));
    m_artworkSide->setCurrentIndex(m_artworkSide->findData(int(spec.artworkSide)));
    m_building = false;
    refresh();
    updateStatusLabel();
}

void MainWindow::artworkKindChanged()
{
    const int kind = m_artworkKind->currentData().toInt();
    const bool isPicture = kind == -1;
    const bool none = kind == int(CodeType::None);
    m_artworkBrowse->setVisible(isPicture);
    m_artworkData->setEnabled(!none);
    m_artworkData->setPlaceholderText(isPicture ? tr("path to the picture")
                                                : tr("content of the code"));
    m_artworkSide->setEnabled(!none);
    if (isPicture && m_artworkData->text().isEmpty())
        chooseArtwork();
    refresh();
}

void MainWindow::chooseArtwork()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose a picture"), QDir::homePath(),
        tr("Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.svg)"));
    if (!path.isEmpty()) {
        m_artworkData->setText(path);
        refresh();
    }
}

void MainWindow::savePdf()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save label"),
        QDir::homePath() + QStringLiteral("/label.pdf"), QStringLiteral("PDF (*.pdf)"));
    if (path.isEmpty())
        return;
    writePdf(path, m_spec, m_layout);
    statusBar()->showMessage(tr("saved: %1").arg(path), 6000);
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
        m_font->setCurrentFont(QFont(tr("DejaVu Sans")));
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
