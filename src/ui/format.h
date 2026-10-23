#pragma once
#include <QDoubleSpinBox>
#include <QString>
#include <cmath>
namespace scalper
{
inline QString displayNumber(double value, int decimals = 6)
{
    if (!std::isfinite(value))
        return QString::fromUtf8("—");
    if (value == 0)
        return "0";
    if (std::abs(value) < std::pow(10.0, -decimals))
        return QString::number(value, 'g', 4);
    auto text = QString::number(value, 'f', decimals);
    if (text.contains('.'))
    {
        while (text.endsWith('0'))
            text.chop(1);
        if (text.endsWith('.'))
            text.chop(1);
    }
    return text == "-0" ? "0" : text;
}
class CompactSpinBox : public QDoubleSpinBox
{
  public:
    using QDoubleSpinBox::QDoubleSpinBox;

  protected:
    QString textFromValue(double value) const override
    {
        return displayNumber(value, decimals());
    }
};
} // namespace scalper
