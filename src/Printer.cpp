#include "Printer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QThread>

namespace ptouch {
namespace {

constexpr int PollIntervalMs = 1000;
constexpr int TimeoutSeconds = 180;

QString runCommand(const QString &program, const QStringList &args,
                   int *exitCode = nullptr, int timeoutMs = 20000)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        if (exitCode)
            *exitCode = -1;
        return {};
    }
    if (exitCode)
        *exitCode = p.exitCode();
    return QString::fromLocal8Bit(p.readAll());
}

// "request id is PT-Label-17 (1 file(s))", wording differs between locales
QString parseJobId(const QString &output)
{
    static const QRegularExpression re(QStringLiteral("(\\S+-\\d+)"));
    const auto m = re.match(output);
    return m.hasMatch() ? m.captured(1) : QString();
}

QStringList lpArguments(const Spec &spec, const PageSize &page,
                        const QString &printer, const QString &pdfPath)
{
    // Mirroring is already baked into the PDF so the preview stays truthful.
    return {QStringLiteral("-d"), printer,
            QStringLiteral("-n"), QString::number(std::max(1, spec.copies)),
            QStringLiteral("-o"), QStringLiteral("PageSize=Custom.%1x%2")
                                      .arg(page.widthPt).arg(page.heightPt),
            QStringLiteral("-o"), QStringLiteral("MirrorPrint=False"),
            QStringLiteral("-o"), QStringLiteral("AutoCut=%1")
                                      .arg(spec.autocut ? QStringLiteral("True")
                                                        : QStringLiteral("False")),
            pdfPath};
}

QString tempPdfPath()
{
    QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/ptouch-XXXXXX.pdf"));
    tmp.setAutoRemove(false);
    tmp.open();
    const QString path = tmp.fileName();
    tmp.close();
    return path;
}

bool jobStillQueued(const QString &printer, const QString &jobId)
{
    const QString out = runCommand(QStringLiteral("lpstat"),
                                   {QStringLiteral("-W"), QStringLiteral("not-completed"),
                                    QStringLiteral("-o"), printer});
    return out.contains(jobId);
}

QString queueTrouble(const QString &printer)
{
    const QString out = runCommand(QStringLiteral("lpstat"), {QStringLiteral("-p"), printer});
    if (out.contains(QStringLiteral("disabled")) || out.contains(QStringLiteral("deaktiviert")))
        return QStringLiteral("The queue was stopped because the printer could not be "
                              "reached. Re-enable it with \"cupsenable %1\".").arg(printer);
    return {};
}

} // namespace

PrintJob::PrintJob(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(PollIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &PrintJob::poll);
}

void PrintJob::start(const Spec &spec, const Layout &layout, const QString &printer)
{
    if (running())
        return;
    if (printer.isEmpty()) {
        emit finished(false, QStringLiteral("No print queue configured."));
        return;
    }

    m_printer = printer;
    m_copies = std::max(1, spec.copies);
    m_elapsed = 0;
    m_pdfPath = tempPdfPath();

    const PageSize page = writePdf(m_pdfPath, spec, layout);
    emit progress(QStringLiteral("Submitting job …"));

    int code = 0;
    const QString out = runCommand(QStringLiteral("lp"),
                                   lpArguments(spec, page, printer, m_pdfPath), &code);
    if (code != 0) {
        done(false, out.trimmed().isEmpty()
                        ? QStringLiteral("lp refused the job.")
                        : out.trimmed());
        return;
    }

    m_jobId = parseJobId(out);
    emit progress(m_copies > 1
                      ? QStringLiteral("Printing %1 labels …").arg(m_copies)
                      : QStringLiteral("Printing …"));
    m_timer.start();
}

void PrintJob::poll()
{
    m_elapsed += PollIntervalMs / 1000;

    if (!m_jobId.isEmpty() && jobStillQueued(m_printer, m_jobId)) {
        if (m_elapsed == 20)
            emit progress(QStringLiteral("Printer has not responded yet — switched on?"));
        if (m_elapsed >= TimeoutSeconds)
            done(false, QStringLiteral("The job has been queued for %1 s. Is the printer "
                                       "switched on and in range?").arg(m_elapsed));
        return;
    }

    const QString trouble = queueTrouble(m_printer);
    if (!trouble.isEmpty()) {
        done(false, trouble);
        return;
    }
    done(true, m_copies > 1 ? QStringLiteral("%1 labels printed.").arg(m_copies)
                            : QStringLiteral("Label printed."));
}

void PrintJob::done(bool ok, const QString &message)
{
    m_timer.stop();
    if (!m_pdfPath.isEmpty()) {
        QFile::remove(m_pdfPath);
        m_pdfPath.clear();
    }
    emit finished(ok, message);
}

PrintResult printAndWait(const Spec &spec, const Layout &layout, const QString &printer,
                         int timeoutSeconds)
{
    PrintResult result;
    result.lengthPt = int(qRound(layout.lengthPt));

    if (printer.isEmpty()) {
        result.message = QStringLiteral("No print queue configured. "
                                        "Run \"ptouch-studio setup\" first.");
        return result;
    }

    const QString pdf = tempPdfPath();
    const PageSize page = writePdf(pdf, spec, layout);

    int code = 0;
    const QString out = runCommand(QStringLiteral("lp"),
                                   lpArguments(spec, page, printer, pdf), &code);
    if (code != 0) {
        QFile::remove(pdf);
        result.message = out.trimmed();
        return result;
    }

    const QString jobId = parseJobId(out);
    QElapsedTimer timer;
    timer.start();
    while (!jobId.isEmpty() && jobStillQueued(printer, jobId)
           && timer.elapsed() < timeoutSeconds * 1000) {
        QThread::msleep(500);
    }
    QFile::remove(pdf);

    const QString trouble = queueTrouble(printer);
    if (!trouble.isEmpty()) {
        result.message = trouble;
        return result;
    }
    if (!jobId.isEmpty() && jobStillQueued(printer, jobId)) {
        result.message = QStringLiteral("Job is still queued — is the printer switched on?");
        return result;
    }

    result.ok = true;
    result.message = spec.copies > 1
                         ? QStringLiteral("%1 labels printed").arg(spec.copies)
                         : QStringLiteral("label printed");
    return result;
}

} // namespace ptouch
