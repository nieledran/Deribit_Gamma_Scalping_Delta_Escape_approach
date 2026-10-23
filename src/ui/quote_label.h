#pragma once
#include "ui/format.h"
#include "ui/stable_label.h"
#include <QPainter>
#include <QVariantAnimation>
#include <algorithm>
namespace scalper
{
class QuoteLabel : public StableLabel
{
  public:
    QuoteLabel(QString title, QColor tint, QWidget* parent = nullptr)
        : StableLabel(parent), title(std::move(title)), tint(std::move(tint)), fade(this)
    {
        setMargin(2);
        fade.setDuration(650);
        fade.setEndValue(0.0);
        connect(&fade, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& value)
                {
                    alpha = value.toDouble();
                    update();
                });
        setQuote(0);
    }
    void setQuote(double value)
    {
        const QString text =
            title + ": " + (value > 0 ? displayNumber(value, 2) : QString::fromUtf8("—"));
        if (text != this->text())
            setText(text);
        if (value > 0 && previous > 0 && value != previous)
        {
            const double bps = std::abs(value - previous) / previous * 10000;
            const double strength = std::clamp(18.0 + bps * 15, 18.0, 100.0);
            fade.stop();
            fade.setStartValue(std::max(alpha, strength));
            fade.start();
        }
        if (value <= 0)
        {
            fade.stop();
            alpha = 0;
            update();
        }
        previous = value;
    }
    double flashStrength() const
    {
        return alpha;
    }

  protected:
    void paintEvent(QPaintEvent* event) override
    {
        if (alpha > 0)
        {
            QPainter painter(this);
            auto color = tint;
            color.setAlpha(int(alpha));
            painter.fillRect(rect(), color);
        }
        StableLabel::paintEvent(event);
    }

  private:
    QString title;
    QColor tint;
    QVariantAnimation fade;
    double previous = 0, alpha = 0;
};
} // namespace scalper
