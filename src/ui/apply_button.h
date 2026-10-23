#pragma once
#include <QPushButton>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QTimer>
namespace scalper
{
class ApplyButton : public QPushButton
{
  public:
    explicit ApplyButton(QWidget* parent = nullptr)
        : QPushButton("Apply", parent), stripes(this), normal(palette())
    {
        stripes.setInterval(100);
        connect(&stripes, &QTimer::timeout, this,
                [this]
                {
                    offset = (offset + 1) % 24;
                    update();
                });
    }
    void setPending(bool value)
    {
        if (pending == value)
            return;
        pending = value;
        if (pending)
        {
            auto blue = normal;
            blue.setColor(QPalette::ButtonText, QColor("#1d4ed8"));
            blue.setColor(QPalette::Button, QColor("#dbeafe"));
            setPalette(blue);
        }
        else
            setPalette(normal);
        setProperty("pendingChanges", value);
        setToolTip(value ? "Settings have changed. Apply to save them before starting." : "");
        animate();
        update();
    }

  protected:
    void showEvent(QShowEvent* event) override
    {
        QPushButton::showEvent(event);
        animate();
    }
    void hideEvent(QHideEvent* event) override
    {
        stripes.stop();
        QPushButton::hideEvent(event);
    }
    void paintEvent(QPaintEvent* event) override
    {
        if (!pending)
        {
            QPushButton::paintEvent(event);
            return;
        }
        QStylePainter painter(this);
        QStyleOptionButton option;
        initStyleOption(&option);
        painter.drawControl(QStyle::CE_PushButtonBevel, option);
        painter.save();
        painter.setClipRect(rect().adjusted(3, 3, -3, -3));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(37, 99, 235, 18));
        for (int x = -height() - 24 + offset; x < width(); x += 24)
            painter.drawPolygon(QPolygon{QPoint(x, height()), QPoint(x + 10, height()),
                                         QPoint(x + height() + 10, 0), QPoint(x + height(), 0)});
        painter.restore();
        painter.drawControl(QStyle::CE_PushButtonLabel, option);
    }

  private:
    void animate()
    {
        if (pending && isVisible())
            stripes.start();
        else
            stripes.stop();
    }
    QTimer stripes;
    QPalette normal;
    bool pending = false;
    int offset = 0;
};
} // namespace scalper
