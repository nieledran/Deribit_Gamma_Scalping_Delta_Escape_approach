#pragma once
#include <QLabel>
#include <QPainter>
namespace scalper
{
// A changing value must never contribute its current text width to the layout.
// The parent grid allocates each field independently of incoming market data.
class StableLabel : public QLabel
{
  public:
    explicit StableLabel(QWidget* parent = nullptr) : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    QSize sizeHint() const override
    {
        return {0, fontMetrics().height() + 4};
    }
    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setFont(font());
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                       QPalette::WindowText));
        painter.drawText(contentsRect(), alignment() | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};
} // namespace scalper
