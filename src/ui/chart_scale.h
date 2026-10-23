#pragma once
#include "ui/format.h"
#include <algorithm>
#include <cmath>
namespace scalper
{
inline double roundAxisStep(double required)
{
    if (!(required > 0) || !std::isfinite(required))
        return 1;
    const double magnitude = std::pow(10.0, std::floor(std::log10(required)));
    const double normalized = required / magnitude;
    for (double step : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (normalized <= step * (1 + 1e-12))
            return step * magnitude;
    return 10 * magnitude;
}
inline double zeroCenteredLimit(double maximumAbsolute, double previousLimit = 0)
{
    if (previousLimit > 0 && maximumAbsolute <= previousLimit)
        return previousLimit;
    return 2 * roundAxisStep(maximumAbsolute > 0 ? maximumAbsolute * 1.05 / 2 : .5);
}
struct RoundedAxis
{
    double lower = 0, upper = 0, step = 0;
};
inline RoundedAxis pnlAxis(double minimum, double maximum, RoundedAxis previous = {})
{
    minimum = std::min(0.0, minimum);
    maximum = std::max(0.0, maximum);
    const bool sameZeroPlacement =
        (minimum >= 0) == (previous.lower >= 0) && (maximum <= 0) == (previous.upper <= 0);
    if (previous.step > 0 && sameZeroPlacement && minimum >= previous.lower &&
        maximum <= previous.upper)
        return previous;
    const double requiredLower = minimum * 1.05, requiredUpper = maximum * 1.05;
    double step = roundAxisStep((requiredUpper - requiredLower) / 4);
    for (;;)
    {
        const double lower =
            maximum <= 0 && minimum < 0 ? -4 * step : std::floor(requiredLower / step) * step;
        const double upper = lower + 4 * step;
        if (upper >= requiredUpper - step * 1e-9)
            return {lower, upper, step};
        step = roundAxisStep(step * 1.01);
    }
}
inline QString axisLabel(double value, double step)
{
    if (std::abs(value) < step * 1e-9)
        return "0";
    const int decimals = std::max(0, int(-std::floor(std::log10(step))) + 1);
    return displayNumber(value, std::min(decimals, 12));
}
} // namespace scalper
