// Setup wizard: checks every step and carries it out at the press of a button.
#pragma once

#include <QDialog>
#include <QThread>
#include <QList>

#include "Provision.h"

class QLabel;
class QComboBox;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace ptouch {

class SetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit SetupDialog(QWidget *parent = nullptr);

private slots:
    void recheck();
    void scan();
    void pair();
    void runSystemSetup();

private:
    // Everything the wizard needs to know about the chosen printer, whichever
    // way it is attached.
    struct Choice {
        bool usb = false;
        QString mac;        // Bluetooth
        QString uri;        // USB
        QString model;
        QString device;     // /dev/usb/lpN, USB only
        bool valid() const { return usb ? !uri.isEmpty() : !mac.isEmpty(); }
    };
    Choice currentChoice() const;

    // The Bluetooth scan takes a dozen seconds; running it here would freeze the
    // dialog, so it happens on a thread.
    class ScanWorker;
    ScanWorker *m_scanWorker = nullptr;
    bool m_working = false;

private:
    void appendLog(const QString &line);
    void setWorking(bool working, const QString &what = QString());
    void updateView();

    SetupState m_state;
    QList<Device> m_devices;
    QList<UsbPrinter> m_usbPrinters;

    QLabel *m_summary = nullptr;
    QLabel *m_steps = nullptr;
    QComboBox *m_deviceBox = nullptr;
    QPushButton *m_scanButton = nullptr;
    QPushButton *m_pairButton = nullptr;
    QPushButton *m_systemButton = nullptr;
    QPushButton *m_recheckButton = nullptr;
    QProgressBar *m_progress = nullptr;
    QPlainTextEdit *m_log = nullptr;
};

} // namespace ptouch
