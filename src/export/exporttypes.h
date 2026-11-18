#pragma once

#include <QString>
#include <functional>

namespace yagf::processing {

struct ExportResult {
    enum class Status : quint8 { Success, Cancelled, Error } status = Status::Error;
    QString error;
};

using ExportProgress = std::function<void(qsizetype completed, qsizetype total)>;

} // namespace yagf::processing
