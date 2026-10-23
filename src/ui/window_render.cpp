#include "ui/format.h"
#include "ui/theme.h"
#include "ui/window.h"
#include <QElapsedTimer>
#include <QTextCursor>
#include <QTimeZone>
#include <algorithm>
namespace scalper
{
static void label(QLabel* widget, const QString& text)
{
    if (widget->text() != text)
    {
        widget->setText(text);
        widget->setToolTip(text);
    }
}
static void cell(QTableWidget* t, int row, int col, const QString& text, bool numeric = false)
{
    auto* item = t->item(row, col);
    if (!item)
    {
        item = new QTableWidgetItem;
        t->setItem(row, col, item);
    }
    if (item->text() != text)
        item->setText(text);
    item->setTextAlignment((numeric ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
}
void Window::render()
{
    QElapsedTimer elapsed;
    elapsed.start();
    TradingSnapshot s;
    AnalyticsSnapshot a;
    RiskResult result;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        s = shared->trading;
        a = shared->analytics;
        if (riskDialog)
            result = shared->risk;
    }
    if (renderedProfile != s.profileId)
    {
        renderedProfile = s.profileId;
        loaded = false;
        dirty = false;
    }
    if (!loaded && (!s.profileId.isEmpty() || demo))
    {
        populateConfig(s.config);
        draftSelected = s.config.selected;
        loaded = true;
    }
    if (!dirty)
        draftSelected = s.config.selected;
    rendering = true;
    if (dirty)
    {
        auto applied = s.config;
        // A remembered threshold is not an effective setting while the stop is off.
        if (!applied.marginLimitEnabled)
            applied.maxMarginUsage = 0;
        if (compact(configFromUi().json()) == compact(applied.json()))
            dirty = false;
    }
    QVector<Option> selected;
    for (const auto& o : s.positions)
        if (s.config.selected.contains(o.spec.name))
            selected.append(o);
    const auto g = aggregate(selected);
    double netDelta = g.delta + s.inventory;
    if (std::abs(netDelta) <=
        8 * std::numeric_limits<double>::epsilon() * (std::abs(g.delta) + std::abs(s.inventory)))
        netDelta = 0;
    label(status, s.connectionStatus);
    const qint64 now = utcNow();
    label(dataAge, s.connected && s.quote.at > 0
                       ? QString("Data age: %1 ms").arg(std::max<qint64>(0, now - s.quote.at))
                       : QString::fromUtf8("Data age: — ms"));
    if (s.connected && s.exchangeClockOffsetMs)
        label(clockDifference, QString("Clock: exchange %1 %2 ms")
                                   .arg(*s.exchangeClockOffsetMs >= 0 ? "ahead" : "behind")
                                   .arg(std::abs(*s.exchangeClockOffsetMs)));
    else
        label(clockDifference, QString::fromUtf8("Clock: —"));
    clockDifference->setToolTip("Exchange clock versus PC, estimated by public/get_time. "
                                "This is a clock difference, not network latency.");
    bidQuote->setQuote(s.quote.bid);
    askQuote->setQuote(s.quote.ask);
    label(latency, s.connected && s.latencyMs >= 0 ? QString("Latency: %1 ms").arg(s.latencyMs)
                                                   : QString::fromUtf8("Latency: — ms"));
    const auto latencyStyle = s.connectionSlow ? QString(theme::AlertStyle) : QString();
    if (latency->styleSheet() != latencyStyle)
        latency->setStyleSheet(latencyStyle);
    const bool ready = s.synced || demo;
    const bool greeksReady = !selected.isEmpty() && selected.size() == s.config.selected.size() &&
                             std::all_of(selected.begin(), selected.end(), [](const Option& o)
                                         { return o.hasGreeks || std::abs(o.quantity) < Epsilon; });
    const QString missing = QString::fromUtf8("—");
    QString state = s.running ? "Running" : s.status == "Stopped" ? "Stopped" : "Paused";
    QString activityText;
    if (s.running)
    {
        if (s.busy)
            activityText = "Executing";
        else if (s.cooldownUntil > utcNow())
            activityText = "Cooldown";
        else
            activityText = "Waiting for escape";
    }
    label(risk, "Price: " + (s.quote.mid() > 0 ? displayNumber(s.quote.mid(), 2) : missing));
    label(estimate, "Bot: " + state);
    label(netDeltaValue,
          QString("Net delta (%1): %2")
              .arg(s.config.base(), ready && greeksReady ? displayNumber(netDelta) : missing));
    label(hedgeValue, QString("Hedge (%1): %2")
                          .arg(s.config.base(), ready ? displayNumber(s.inventory) : missing));
    const bool knownHedgeValue = ready && s.quote.mid() > 0;
    const double usedNotional = std::abs(s.inventory) * s.quote.mid();
    label(hedgeUsage, QString("Target: %1 | Left: %2 %3")
                          .arg(ready ? displayNumber(s.target) : missing,
                               ready ? displayNumber(s.target - s.inventory) : missing,
                               s.config.base()));
    hedgeUsage->setToolTip("Target at the applied delta coverage. Actual hedge notional: " +
        (knownHedgeValue ? "$" + displayNumber(usedNotional, 2) : missing));
    label(normalizedValue, "Normalized delta: " +
                               (ready && greeksReady ? displayNumber(g.normalized(), 4) : missing));
    label(referenceValue, "Reference delta: " +
                              (ready && s.anchored ? displayNumber(s.referenceDelta, 4) : missing));
    label(escapeValue, "Escape threshold: " + displayNumber(s.config.deltaChange, 4));
    label(legsValue, QString("%1 options + %2 perpetual")
                         .arg(s.config.selected.size()).arg(std::abs(s.inventory) > Epsilon ? 1 : 0));
    label(executionState, activityText);
    const qint64 cooldownLeft = s.running ? std::max<qint64>(0, s.cooldownUntil - now) : 0;
    label(activity,
          "Cooldown left: " + displayNumber(std::ceil(cooldownLeft / 100.0) / 10, 1) + " s");
    const bool valued = a.ready && a.timestamp > 0 && a.constructionId == s.constructionId;
    const bool staleValuation = !demo && (!s.connected || !s.quote.fresh(now) ||
                                          now - a.timestamp > Time::MarketDataMaxAgeMs);
    label(netPnl, "Net P&L: " + (valued ? displayNumber(a.pnl.net(), 2) : missing));
    QStringList coverage;
    if (valued && !a.complete)
        coverage << "Incomplete";
    if (valued && staleValuation)
        coverage << "Last valuation";
    label(pnlCoverage, coverage.join(" | "));
    pnlCoverage->setToolTip(
        a.completeness +
        (staleValuation ? "\nShowing the last valuation; prices are not fresh." : ""));
    netPnl->setToolTip(a.completeness + (a.startedAt ? "\nSince " + isoUtc(a.startedAt) : ""));
    if (tabs->currentWidget() == positions)
    {
        const bool hasHedge = std::abs(s.inventory) > Epsilon;
        positions->setRowCount(selected.size() + (hasHedge ? 1 : 0));
        for (int row = 0; row < selected.size(); ++row)
        {
            const auto& o = selected[row];
            cell(positions, row, 0, o.spec.name);
            positions->item(row, 0)->setToolTip(o.spec.name);
            cell(positions, row, 1, displayNumber(o.quantity), true);
            cell(positions, row, 2, o.hasGreeks ? displayNumber(o.quantity * o.delta) : missing,
                 true);
            cell(positions, row, 3, o.hasGreeks ? displayNumber(o.quantity * o.gamma, 9) : missing,
                 true);
            cell(positions, row, 4, o.hasGreeks ? displayNumber(o.quantity * o.theta, 3) : missing,
                 true);
            cell(positions, row, 5, o.hasGreeks ? displayNumber(o.quantity * o.vega, 3) : missing,
                 true);
            cell(positions, row, 6,
                 o.hasGreeks ? displayNumber(o.mark, o.spec.inverse ? 6 : 2) : missing, true);
            positions->item(row, 6)->setToolTip("Mark in " + o.spec.settlement);
            const QString dataState = std::abs(o.quantity) < Epsilon      ? "Closed"
                                      : o.spec.expiry <= utcNow()         ? "Expired"
                                      : (demo || o.freshGreeks(utcNow())) ? "Live"
                                                                          : "Stale";
            cell(positions, row, 7, dataState);
            positions->item(row, 7)->setToolTip(
                o.tickerAt > 0 ? QString("Last option update: %1 | age %2 ms")
                                     .arg(isoUtc(o.tickerAt))
                                     .arg(std::max<qint64>(0, now - o.tickerAt))
                               : "No option quote received yet");
        }
        if (hasHedge)
        {
            const int row = selected.size();
            cell(positions, row, 0, s.config.instrument);
            cell(positions, row, 1, ready ? displayNumber(s.inventory) : missing, true);
            cell(positions, row, 2, ready ? displayNumber(s.inventory) : missing, true);
            for (int col = 3; col <= 5; ++col)
                cell(positions, row, col, "0", true);
            cell(positions, row, 6, s.quote.mark > 0 ? displayNumber(s.quote.mark, 2) : missing, true);
            cell(positions, row, 7, ready && s.quote.fresh(now) ? "Live" : "Stale");
            positions->item(row, 0)->setToolTip("Actual bot perpetual hedge; delta equals signed quantity.");
        }
    }
    if (tabs->currentWidget() == orders)
    {
        const int first = std::max(0, int(s.orders.size()) - 20);
        orders->setRowCount(s.orders.size() - first);
        for (int i = first; i < s.orders.size(); ++i)
        {
            const auto& o = s.orders[i];
            const int row = i - first;
            cell(orders, row, 0, o.buy ? "BUY" : "SELL");
            orders->item(row, 0)->setToolTip(o.label);
            cell(orders, row, 1, displayNumber(o.price, 2), true);
            cell(orders, row, 2, displayNumber(o.total), true);
            cell(orders, row, 3, displayNumber(o.filled), true);
            cell(orders, row, 4, o.ioc ? "Limit IOC" : o.postOnly ? "Post-only GTC" : "Limit GTC");
            cell(orders, row, 5,
                 o.uncertain             ? "Uncertain"
                 : !o.mutation.isEmpty() ? o.mutation
                 : o.state);
        }
    }
    if (tabs->currentWidget() == journal)
    {
        auto text = s.messages.join('\n');
        if (journal->toPlainText() != text)
        {
            journal->setPlainText(text);
            journal->moveCursor(QTextCursor::End);
        }
    }
    QString reason = demo ? "Offline demo: synthetic positions; exchange connection disabled."
                     : !s.problem.isEmpty()        ? s.problem
                     : !s.connected && !demo       ? "Enter API credentials and click Connect."
                     : !s.synced && !demo          ? "Synchronizing positions and bot orders..."
                     : s.config.selected.isEmpty() ? "Select option positions, then Apply settings."
                     : !demo && !s.quote.fresh(now)
                         ? "Waiting for fresh bid/ask prices; trading is paused."
                     : !a.message.isEmpty() ? a.message
                     : !valued              ? "Waiting for option prices to calculate P&L."
                     : !a.complete  ? "P&L history incomplete; open Details for coverage."
                     : s.running            ? "Trading active."
                                            : "Ready. Press Start / Resume to hedge.";
    label(quality, reason);
    quality->setToolTip(reason + "\n" + a.historyStatus);
    auto qualityStyle = !s.problem.isEmpty() ? QString(theme::AlertStyle) : QString();
    if (quality->styleSheet() != qualityStyle)
        quality->setStyleSheet(qualityStyle);
    if (advancedDialog->isVisible())
        label(marginInfo,
              s.margin.valid
                  ? QString("Model: %1\n%2\nMM usage: %3% | User stop: %4")
                            .arg(s.margin.model, s.margin.scope,
                                 displayNumber(s.margin.usage() * 100, 2),
                                 s.config.marginLimitEnabled ? "enabled" : "off") +
                        QString("\nDiagnostics: UI %1 µs | Analytics %2 µs\nRPC queue %3 ms | Last "
                                "hedge %4 ms")
                            .arg(lastRenderUs)
                            .arg(a.calculationUs)
                            .arg(s.rpcQueueMs < 0 ? missing : QString::number(s.rpcQueueMs),
                                 s.lastHedgeMs < 0 ? missing : QString::number(s.lastHedgeMs))
                  : "Account margin data unavailable");
    if (pnlDialog && pnlDialog->isVisible())
    {
        pnl->setRowCount(1);
        const auto& v = a.pnl;
        cell(pnl, 0, 0, "Construction");
        const double nums[] = {v.options, v.realized, v.unrealized, v.optionFees + v.hedgeFees,
                               v.funding, v.net()};
        for (int col = 1; col < 7; ++col)
            cell(pnl, 0, col, valued ? displayNumber(nums[col - 1], col == 5 ? 4 : 2) : missing, true);
        cell(pnl, 0, 7, !valued ? "Waiting for first Start"
                       : a.complete ? "Since " + isoUtc(a.startedAt) : "Incomplete");
    }
    if (riskDialog && result.id == riskRequestId && displayedRiskId != result.id)
    {
        displayedRiskId = result.id;
        chart->setData(result);
        label(riskTimestamp, result.problem.isEmpty()
                                 ? "Snapshot: " + isoUtc(result.timestamp) +
                                       (result.complete ? "" : " | Incomplete P&L history")
                                 : result.problem);
        QStringList ivs, fullIvs;
        for (const auto& value : result.impliedVols)
        {
            const auto percent = displayNumber(value.second * 100, 2) + "%";
            ivs << value.first.section('-', 1) + ": " + percent;
            fullIvs << value.first + ": " + percent;
        }
        label(riskIv, ivs.isEmpty() ? QString("IV: unavailable")
                                    : "IV (implied volatility): " + ivs.join(" | "));
        riskIv->setToolTip(fullIvs.join('\n'));
        riskRefresh->setEnabled(true);
    }
    applyButton->setEnabled(!demo && s.synced && !s.running && !s.busy);
    label(status, s.connectionStatus);
    applyButton->setPending(dirty);
    startButton->setEnabled(!demo && s.synced && !s.running && !s.busy && !dirty);
    stopButton->setEnabled(!demo && s.connected);
    selectButton->setEnabled(!s.running && !s.busy);
    closeAction->setEnabled(!demo && s.synced && std::abs(s.inventory) > Epsilon && !s.busy);
    deleteAction->setEnabled(!demo && s.canDelete);
    closeAction->setToolTip(s.positionMismatch
                                ? "Positions differ after an external trade. Export CSV, then "
                                  "Delete local profile to restart."
                                : "Close the bot's verified perpetual hedge; options remain open.");
    closeAction->setText(closeAction->isEnabled() ? "Close hedge" : "Close hedge (Locked)");
    deleteAction->setText(deleteAction->isEnabled() ? "Delete local profile"
                                                    : "Delete local profile (Locked)");
    connectButton->setEnabled(!demo && !s.running);
    id->setEnabled(!demo && !s.running);
    secret->setEnabled(!demo && !s.running);
    rendering = false;
    lastRenderUs = elapsed.nsecsElapsed() / 1000;
    ++renderCount;
}
} // namespace scalper
