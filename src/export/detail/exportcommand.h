#pragma once

#include "exporttypes.h"
#include <QStringList>
#include <atomic>

namespace yagf::processing::detail {

// Run one export helper with cancellation, timeout, and argument-list invocation.
ExportResult runExportCommand(const QString &program, const QStringList &arguments,
                              const std::atomic_bool &cancelled, const QString &workingDirectory = {});

} // namespace yagf::processing::detail
