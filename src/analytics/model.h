#pragma once
#include "engine/domain.h"
namespace scalper
{
struct ModelValue
{
    double usd = 0, delta = 0;
};
ModelValue optionModel(const Option& option, double scenarioIndex, qint64 now);
Greeks modelGreeks(const Option& option, double scenarioIndex, qint64 now);
QVector<ProfilePoint> portfolioCurve(const QVector<Option>& options, double hedge,
                                     double currentHedgePrice, double currentNetPnl, qint64 now,
                                     double range = .15, int count = 241);
std::optional<double> escapePrice(const QVector<Option>& options, double reference, double band,
                                  double currentPrice, qint64 now, bool above);
} // namespace scalper
