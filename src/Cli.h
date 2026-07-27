#pragma once

#include <QStringList>

namespace ptouch {

// Sub-commands without an interface. Returns the process exit code.
int runCli(const QStringList &arguments);

// Help text for `ptouch-studio --help`.
QString usage();

} // namespace ptouch
