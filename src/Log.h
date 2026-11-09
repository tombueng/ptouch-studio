// Session log and diagnostics bundle.
//
// Everything the application does to find, set up, query and print to a printer
// is written here — timestamped, to an in-memory buffer and to a rotating file on
// disk. The point is the hard cases: when a label comes out blank or the printer
// will not settle, "Help → Save diagnostics report …" (or the `ptouch-studio
// diagnostics` command) gathers this log together with the machine's CUPS,
// Bluetooth and driver state into one text file to attach to a bug report.
#pragma once

#include <QString>

namespace ptouch {
namespace log {

// The privileged CUPS backend runs as root under cupsd and cannot write to the
// user's session log, so it appends its own trace (bytes sent, every status reply
// from the printer) here as a world-readable file. collectDiagnostics() folds it
// back in. CupsBackend.cpp hard-codes the same path — keep the two in sync.
constexpr const char *BackendLogPath = "/var/log/ptouch-studio-backend.log";

// Opens the log file and installs the Qt message handler. Call once at startup;
// writes a header describing the build and this run. `invocation` is the command
// line, so the log says what the run was asked to do.
void init(const QString &invocation);

// Appends one timestamped line to the session log (file + memory buffer).
void line(const QString &message);

// Convenience: "<key>: <value>" on one line.
void field(const QString &key, const QString &value);

// A labelled divider that marks the start of an activity (a scan, a print).
void section(const QString &title);

// Logs a block of text verbatim (e.g. a sub-process's output), indented, under an
// optional caption. Empty blocks are skipped.
void block(const QString &caption, const QString &text);

// Where the log file lives, e.g. ~/.local/state/ptouch-studio/session.log.
QString filePath();

// The session log as text, read back from the file so it spans earlier runs up to
// the rotation limit.
QString sessionText();

// A full diagnostics report: build, OS, Bluetooth, CUPS / driver / queue / PPD,
// RFCOMM state, the privileged CUPS backend's own log and the session log. This is
// the text a user sends us to debug a print that misbehaves.
QString collectDiagnostics();

} // namespace log
} // namespace ptouch
