#pragma once

#include <QListWidget>

namespace yagf::ui {

class BlockMiniatureList final : public QListWidget {
    Q_OBJECT
public:
    explicit BlockMiniatureList(QWidget *parent = nullptr);
signals:
    void blockContextMenuRequested(int index, const QPoint &globalPosition);
protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
};

} // namespace yagf::ui
