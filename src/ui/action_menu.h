#pragma once
#include "ui/theme.h"
#include <QMenu>
#include <QStyleOptionMenuItem>
#include <QStylePainter>
namespace scalper
{
// Keep disabled destructive actions legible; QAction itself remains disabled.
class ActionMenu : public QMenu
{
  public:
    using QMenu::QMenu;

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        QStyleOption panel;
        panel.initFrom(this);
        painter.drawPrimitive(QStyle::PE_PanelMenu, panel);
        for (auto* action : actions())
        {
            if (!action->isVisible())
                continue;
            QStyleOptionMenuItem item;
            initStyleOption(&item, action);
            item.rect = actionGeometry(action);
            item.font = font();
            item.font.setStrikeOut(false);
            item.font.setItalic(false);
            if (action->property("danger").toBool())
            {
                item.state |= QStyle::State_Enabled;
                item.palette.setColor(QPalette::Text, QColor(theme::Alert));
                item.palette.setColor(QPalette::HighlightedText, QColor(theme::Alert));
            }
            painter.drawControl(QStyle::CE_MenuItem, item);
        }
    }
};
} // namespace scalper
