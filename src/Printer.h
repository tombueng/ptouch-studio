// Handing a job to CUPS and following it through to the end.
//
// The job is not done when `lp` returns — the label still has to travel over the
// Bluetooth link and through the tape feed. The interface needs that real end so
// nobody hits Print several times thinking nothing happened.
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include "Engine.h"

namespace ptouch {

class PrintJob : public QObject {
    Q_OBJECT
public:
    explicit PrintJob(QObject *parent = nullptr);

    void start(const Spec &spec, const Layout &layout, const QString &printer);
    bool running() const { return m_timer.isActive(); }

signals:
    void progress(const QString &message);
    void finished(bool ok, const QString &message);

private:
    void poll();
    void done(bool ok, const QString &message);

    QTimer m_timer;
    QString m_printer;
    QString m_jobId;
    QString m_pdfPath;
    int m_elapsed = 0;
    int m_copies = 1;
};

// Blocking variant for the command line: prints and waits for completion.
struct PrintResult {
    bool ok = false;
    QString message;
    int lengthPt = 0;
};
PrintResult printAndWait(const Spec &spec, const Layout &layout, const QString &printer,
                         int timeoutSeconds = 180);

} // namespace ptouch
