#pragma once

#include <QImage>

namespace yagf::processing {

QByteArray symbolShape(const QImage &page, const QRect &bounds);

} // namespace yagf::processing
