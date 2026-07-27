#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QPointer>

#include "Config.h"
#include "Engine.h"
#include "Printer.h"
#include "Status.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;

namespace ptouch {

class PreviewWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Fills the fields with presentable values for screenshots.
    void demoContent();

protected:
    void changeEvent(QEvent *event) override;

private slots:
    void refresh();
    void tapeChanged();
    void adoptDetectedTape();
    void detectTape(bool quiet = true);
    void statusArrived(const ptouch::Status &status);
    void doPrint();
    void savePdf();
    void openSetup();
    void insertSymbol();

private:
    QWidget *buildControls();
    Spec collectSpec() const;
    void updateStatusLabel();
    void confirmAndPrint();
    void setBusy(bool busy, const QString &message = QString());
    void restoreSettings();
    void storeSettings() const;

    Config m_config;
    Spec m_spec;
    Layout m_layout;

    PreviewWidget *m_preview = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QPushButton *m_symbolButton = nullptr;
    QComboBox *m_tape = nullptr;
    QLabel *m_tapeStatus = nullptr;
    QPushButton *m_adopt = nullptr;
    QFontComboBox *m_font = nullptr;
    QCheckBox *m_bold = nullptr;
    QCheckBox *m_italic = nullptr;
    QCheckBox *m_autoSize = nullptr;
    QDoubleSpinBox *m_size = nullptr;
    QComboBox *m_align = nullptr;
    QDoubleSpinBox *m_lineSpacing = nullptr;
    QCheckBox *m_autoLength = nullptr;
    QDoubleSpinBox *m_length = nullptr;
    QDoubleSpinBox *m_margin = nullptr;
    QCheckBox *m_frame = nullptr;
    QCheckBox *m_mirror = nullptr;
    QCheckBox *m_cut = nullptr;
    QSpinBox *m_copies = nullptr;
    QLabel *m_warning = nullptr;
    QPushButton *m_printButton = nullptr;
    QPushButton *m_pdfButton = nullptr;
    QProgressBar *m_progress = nullptr;

    PrintJob *m_job = nullptr;
    // QPointer, because the worker deletes itself once it is done —
    // a raw pointer would dangle and the destructor could not check it.
    QPointer<StatusWorker> m_worker;
    QTimer *m_poll = nullptr;

    Status m_status;
    double m_detected = 0;          // tape width most recently reported
    QElapsedTimer m_detectedAt;
    bool m_manualTape = false;      // user picked a width themselves
    bool m_settingTape = false;     // suppresses m_manualTape while following along
    bool m_printPending = false;    // check running, print afterwards
    bool m_building = true;
    bool m_demoMode = false;   // screenshots: no printer traffic
};

} // namespace ptouch
