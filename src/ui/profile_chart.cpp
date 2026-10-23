#include "ui/profile_chart.h"
#include "ui/chart_scale.h"
#include "ui/format.h"
#include "ui/theme.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <algorithm>
namespace scalper
{
ProfileChart::ProfileChart(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(450, 240);
    setMouseTracking(true);
    setObjectName("portfolioChart");
}
void ProfileChart::setData(const RiskResult& value)
{
    result = value;
    double pnlMinimum = 0, pnlMaximum = 0;
    std::array<double, 4> magnitudes{};
    for (const auto& point : result.curve)
    {
        pnlMinimum = std::min(pnlMinimum, point.pnl);
        pnlMaximum = std::max(pnlMaximum, point.pnl);
        const std::array<double, 4> greeks{point.delta, point.gamma, point.theta, point.vega};
        for (int i = 0; i < 4; ++i)
            magnitudes[i] = std::max(magnitudes[i], std::abs(greeks[i]));
    }
    const auto axis = pnlAxis(pnlMinimum, pnlMaximum, {minimumPnl, maximumPnl, pnlStep});
    minimumPnl = axis.lower;
    maximumPnl = axis.upper;
    pnlStep = axis.step;
    for (int i = 0; i < 4; ++i)
        greekLimits[i] = zeroCenteredLimit(magnitudes[i], greekLimits[i]);
    update();
}
void ProfileChart::setGreek(ScenarioGreek value)
{
    greek = value;
    update();
}
double ProfileChart::greekValue(const ProfilePoint& point) const
{
    switch (greek)
    {
    case ScenarioGreek::Gamma:
        return point.gamma;
    case ScenarioGreek::Theta:
        return point.theta;
    case ScenarioGreek::Vega:
        return point.vega;
    default:
        return point.delta;
    }
}
QString ProfileChart::greekLabel() const
{
    switch (greek)
    {
    case ScenarioGreek::Gamma:
        return "Gamma (coin/$)";
    case ScenarioGreek::Theta:
        return "Theta ($, API)";
    case ScenarioGreek::Vega:
        return "Vega ($/vol pt)";
    default:
        return "Delta (coin)";
    }
}
void ProfileChart::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().base());
    if (result.curve.isEmpty())
    {
        painter.drawText(rect().adjusted(15, 15, -15, -15), Qt::AlignCenter | Qt::TextWordWrap,
                         result.problem.isEmpty() ? "Waiting for a snapshot" : result.problem);
        return;
    }
    const auto& points = result.curve;
    plot = QRectF(86, 35, std::max(10, width() - 190), std::max(10, height() - 112));
    minimumPrice = points.first().price;
    maximumPrice = points.last().price;
    maximumGreek = greekLimits[int(greek)];
    minimumGreek = -maximumGreek;
    auto x = [&](double value)
    { return plot.left() + (value - minimumPrice) / (maximumPrice - minimumPrice) * plot.width(); };
    auto y = [&](double value)
    { return plot.bottom() - (value - minimumPnl) / (maximumPnl - minimumPnl) * plot.height(); };
    auto yg = [&](double value)
    {
        return plot.bottom() -
               (value - minimumGreek) / (maximumGreek - minimumGreek) * plot.height();
    };
    for (int i = 0; i <= 4; ++i)
    {
        const double t = i / 4.0, yy = plot.bottom() - t * plot.height();
        painter.setPen(QColor(theme::ChartGrid));
        painter.drawLine(QPointF(plot.left(), yy), QPointF(plot.right(), yy));
        painter.setPen(QColor(theme::ChartPnl));
        painter.drawText(QRectF(0, yy - 10, 78, 20), Qt::AlignRight | Qt::AlignVCenter,
                         axisLabel(minimumPnl + i * pnlStep, pnlStep));
        painter.setPen(QColor(theme::ChartDelta));
        painter.drawText(QRectF(plot.right() + 7, yy - 10, 95, 20),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         axisLabel((i - 2) * maximumGreek / 2, maximumGreek / 2));
    }
    const double priceStep = roundAxisStep((maximumPrice - minimumPrice) / 4);
    const double firstTick = std::ceil(minimumPrice / priceStep) * priceStep;
    for (double price = firstTick; price <= maximumPrice + priceStep * 1e-9; price += priceStep)
    {
        const double xx = x(price);
        painter.setPen(QColor(theme::ChartGrid));
        painter.drawLine(QPointF(xx, plot.top()), QPointF(xx, plot.bottom()));
        painter.setPen(QColor(theme::ChartText));
        painter.drawText(QRectF(xx - 50, plot.bottom() + 5, 100, 20), Qt::AlignCenter,
                         axisLabel(price, priceStep));
    }
    painter.save();
    painter.setClipRect(plot);
    painter.setPen(QPen(QColor(theme::ChartZero), 1.5));
    painter.drawLine(QPointF(plot.left(), y(0)), QPointF(plot.right(), y(0)));
    painter.setPen(QPen(QColor(theme::ChartDelta), 1, Qt::DashLine));
    painter.drawLine(QPointF(plot.left(), yg(0)), QPointF(plot.right(), yg(0)));
    QPainterPath pnlPath, greekPath;
    for (int i = 0; i < points.size(); ++i)
    {
        QPointF a(x(points[i].price), y(points[i].pnl)),
            b(x(points[i].price), yg(greekValue(points[i])));
        if (i == 0)
        {
            pnlPath.moveTo(a);
            greekPath.moveTo(b);
        }
        else
        {
            pnlPath.lineTo(a);
            greekPath.lineTo(b);
        }
    }
    painter.setPen(QPen(QColor(theme::ChartPnl), 2));
    painter.drawPath(pnlPath);
    painter.setPen(QPen(QColor(theme::ChartDelta), 2, Qt::DashLine));
    painter.drawPath(greekPath);
    for (auto bound : {result.lower, result.upper})
        if (bound)
        {
            painter.setPen(QPen(QColor(theme::ChartEscape), 1, Qt::DotLine));
            painter.drawLine(QPointF(x(*bound), plot.top()), QPointF(x(*bound), plot.bottom()));
        }
    painter.setPen(QPen(QColor(theme::ChartSpot), 1));
    painter.drawLine(QPointF(x(result.spot), plot.top()), QPointF(x(result.spot), plot.bottom()));
    painter.restore();
    painter.setPen(QColor(theme::ChartPnl));
    painter.drawText(QRectF(0, 8, 100, 20), Qt::AlignLeft, "P&L (USD eq.)");
    painter.setPen(QColor(theme::ChartDelta));
    painter.drawText(QRectF(plot.right(), 8, 104, 20), Qt::AlignLeft, greekLabel());
    painter.setPen(QColor(theme::ChartText));
    painter.drawText(QRectF(0, height() - 50, width(), 20), Qt::AlignCenter,
                     "Underlying price (USD)");
    struct LegendItem
    {
        QString text;
        QColor color;
        Qt::PenStyle style;
        qreal penWidth;
        bool vertical;
    };
    const std::array<LegendItem, 4> legend{
        {{"P&L", QColor(theme::ChartPnl), Qt::SolidLine, 2, false},
         {greekLabel().section(' ', 0, 0), QColor(theme::ChartDelta), Qt::DashLine, 2, false},
         {"Delta triggers", QColor(theme::ChartEscape), Qt::DotLine, 1, true},
         {"Current price", QColor(theme::ChartSpot), Qt::SolidLine, 1, true}}};
    const QFontMetricsF metrics(painter.font());
    constexpr qreal sampleWidth = 28, textGap = 7, itemGap = 22;
    qreal legendWidth = -itemGap;
    for (const auto& item : legend)
        legendWidth += sampleWidth + textGap + metrics.horizontalAdvance(item.text) + itemGap;
    qreal left = (width() - legendWidth) / 2;
    const qreal centerY = height() - 12;
    for (const auto& item : legend)
    {
        painter.setPen(QPen(item.color, item.penWidth, item.style));
        if (item.vertical)
            painter.drawLine(QPointF(left + sampleWidth / 2, centerY - 7),
                             QPointF(left + sampleWidth / 2, centerY + 7));
        else
            painter.drawLine(QPointF(left, centerY), QPointF(left + sampleWidth, centerY));
        const qreal textWidth = metrics.horizontalAdvance(item.text);
        painter.setPen(QColor(theme::ChartText));
        painter.drawText(QRectF(left + sampleWidth + textGap, centerY - 10, textWidth + 2, 20),
                         Qt::AlignLeft | Qt::AlignVCenter, item.text);
        left += sampleWidth + textGap + textWidth + itemGap;
    }
}
void ProfileChart::mouseMoveEvent(QMouseEvent* event)
{
    if (result.curve.isEmpty() || !plot.contains(event->position()))
        return;
    for (const auto& bound : {result.lower, result.upper})
        if (bound && std::abs(event->position().x() -
                              (plot.left() + (*bound - minimumPrice) /
                                                 (maximumPrice - minimumPrice) * plot.width())) < 6)
        {
            QToolTip::showText(event->globalPosition().toPoint(),
                               QString("Modelled delta trigger at $%1\n"
                                       "|Normalized option delta - reference %2| reaches %3.\n"
                                       "Time and each option's IV stay fixed in this snapshot.")
                                   .arg(displayNumber(*bound, 2),
                                        displayNumber(result.referenceDelta, 4),
                                        displayNumber(result.deltaChange, 4)),
                               this);
            return;
        }
    const int i = std::clamp(int(std::round((event->position().x() - plot.left()) / plot.width() *
                                            (result.curve.size() - 1))),
                             0, int(result.curve.size() - 1));
    const auto& point = result.curve[i];
    QToolTip::showText(event->globalPosition().toPoint(),
                       QString("Price: %1\nP&L: %2 USD eq.\n%3: %4")
                           .arg(displayNumber(point.price, 2), displayNumber(point.pnl, 2),
                                greekLabel(), displayNumber(greekValue(point), 9)),
                       this);
}
} // namespace scalper
