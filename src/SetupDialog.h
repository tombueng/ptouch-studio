// Setup wizard: checks every step and carries it out at the press of a button.
#pragma once

#include <QDialog>
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
    void appendLog(const QString &line);
    void setWorking(bool working, const QString &what = QString());
    void updateView();

    SetupState m_state;
    QList<Device> m_devices;

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
