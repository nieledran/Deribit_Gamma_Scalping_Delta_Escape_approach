#include "analytics/model.h"
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
    if (!s.running && s.busy)
    {
        state = "Waiting";
        activityText = s.status;
    }
    if (s.running)
    {
        if (s.status.startsWith("Waiting"))
            activityText = s.status;
        else if (s.busy || s.executing)
            activityText = "Executing";
        else
            activityText = strategyName(s.strategy);
    }
    const bool validPrice =
        s.quote.bid > 0 && s.quote.ask >= s.quote.bid && std::isfinite(s.quote.mid());
    label(risk, "Middle Price: " + (validPrice ? displayNumber(s.quote.mid(), 2) : missing));
    risk->setToolTip("Middle Price = (Bid + Ask) / 2, in USD.");
    const bool freshMetrics =
        ready && greeksReady && validPrice &&
        (demo || (s.connected && s.quote.fresh(now) &&
                  std::all_of(selected.begin(), selected.end(),
                              [now](const Option& o)
                              {
                                  return std::abs(o.quantity) < Epsilon ||
                                         (o.spec.expiry > now && o.freshGreeks(now));
                              })));
    const auto moves =
        freshMetrics ? estimateMoves(g, s.quote.mid(), s.config.deltaChange) : MoveEstimates{};
    label(rebalanceBand,
          "Rebalance band: " +
              (moves.rebalancePercent
                   ? QString::fromUtf8("≈") + displayNumber(*moves.rebalancePercent, 3) + "%"
                   : missing));
    QString bandDetails =
        "Approximate price move from a hedge reference to the next Delta Change trigger, "
        "in either direction. Not the distance left from the current price.\n"
        "100 x Delta Change x gross option quantity / |net option gamma| / Middle Price.\n"
        "Local constant-gamma estimate; time and IV changes can change the result. "
        "Coverage changes trade size, not this price threshold.\n";
    if (freshMetrics && moves.rebalancePercent)
    {
        const double quantity = g.gross * s.config.deltaChange * s.config.hedgePercent / 100;
        const double notional = quantity * s.quote.mid();
        bandDetails += QString("At this band: ~%1 %2 turnover ($%3); one-way commission "
                               "~$%4 maker / ~$%5 taker. Before lot rounding; excludes spread "
                               "and slippage. Strategy orders are post-only; historical fills may include taker fees.")
                           .arg(displayNumber(quantity), s.config.base(),
                                displayNumber(notional, 2), displayNumber(notional * s.makerFee, 4),
                                displayNumber(notional * s.takerFee, 4));
    }
    else
        bandDetails += "Unavailable without fresh prices, complete Greeks and nonzero net gamma.";
    rebalanceBand->setToolTip(bandDetails);
    const bool shortHorizon = std::any_of(
        selected.begin(), selected.end(), [now](const Option& o)
        { return std::abs(o.quantity) > Epsilon && o.spec.expiry - now < Time::DayMs; });
    label(breakEvenMove,
          QString(shortHorizon ? "Break-even (<1d): %1" : "Break-even (daily): %1")
              .arg(moves.breakEven
                       ? QString::fromUtf8("≈") +
                             displayNumber(100 * *moves.breakEven / s.quote.mid(), 3) + "%"
                       : missing));
    breakEvenMove->setToolTip(
        (moves.breakEven ? "USD equivalent: ~$" + displayNumber(*moves.breakEven, 2) + ".\n"
                         : QString()) +
        "Approximate gamma/theta break-even move (%) = "
        "100 x sqrt(2 x |total theta| / total gamma) / Middle Price.\n"
        "Uses signed quantities of selected options; requires positive net gamma and "
        "nonpositive theta. Constant Greeks; excludes fees, spread, funding and IV changes.\n"
        "Deribit caps theta to remaining lifetime below one day. '<1d' marks a basket "
        "containing such an option: this is then a capped-horizon estimate, not a full-day "
        "forecast. Missing or stale inputs are shown as unavailable.");
    label(estimate, "Bot: " + state);
    label(netDeltaValue,
          QString("Net delta (%1): %2")
              .arg(s.config.base(), ready && greeksReady ? displayNumber(netDelta) : missing));
    label(hedgeValue, QString("Futures position: %1 %2")
                          .arg(ready ? displayNumber(s.inventory) : missing, s.config.base()));
    hedgeValue->setToolTip("Actual perpetual quantity. Negative = short; positive = long.");
    const bool knownHedgeValue = ready && s.quote.mid() > 0;
    const double usedNotional = std::abs(s.inventory) * s.quote.mid();
    label(hedgeUsage, QString("Futures target: %1 %2")
                          .arg(ready ? displayNumber(s.target) : missing, s.config.base()));
    hedgeUsage->setToolTip(
        "Required futures position = -option delta x coverage / 100. "
        "This is a quantity, not the total delta of the construction. Notional: " +
        (knownHedgeValue ? "$" + displayNumber(usedNotional, 2) : missing));
    const double adjustment = s.target - s.inventory;
    label(hedgeRemaining, !ready ? "To rebalance: " + missing
                          : std::abs(adjustment) < Epsilon
                              ? "To rebalance: 0 " + s.config.base()
                              : QString("To rebalance: %1 %2 %3")
                                    .arg(adjustment > 0 ? "BUY" : "SELL",
                                         displayNumber(std::abs(adjustment)), s.config.base()));
    hedgeRemaining->setToolTip(
        "Target futures quantity minus actual quantity. Resting orders prepare both Delta Change levels.");
    label(targetDeltaValue,
          QString("Delta goal (%1%): %2 %3")
              .arg(displayNumber(s.config.hedgePercent, 2),
                   ready && greeksReady ? displayNumber(g.delta * (1 - s.config.hedgePercent / 100))
                                        : missing,
                   s.config.base()));
    label(normalizedValue, "Normalized delta: " +
                               (ready && greeksReady ? displayNumber(g.normalized(), 4) : missing));
    label(referenceValue, "Reference delta: " +
                              (ready && s.anchored ? displayNumber(s.referenceDelta, 4) : missing));
    label(legsValue, QString("%1 options + %2 perpetual")
                         .arg(s.config.selected.size())
                         .arg(std::abs(s.inventory) > Epsilon ? 1 : 0));
    label(executionState, activityText);
    executionState->setToolTip(activityText);
    label(activity, s.hedgeAlert.isEmpty()
                        ? QString("Hedge error: %1 %2").arg(displayNumber(s.hedgeQuotes.error), s.config.base())
                        : s.hedgeAlert);
    activity->setToolTip(s.hedgeAlert.isEmpty() ? "Long-gamma options strategy. Passive corrections alert after 30 seconds; no taker fallback." : s.hedgeAlert);
    activity->setStyleSheet(s.hedgeAlert.isEmpty() ? QString() : QString(theme::AlertStyle));
    auto quoteText = [&](HedgeQuote q, bool buy)
    {
        q.buy = buy;
        QString state = q.enabled ? "preparing" : q.reason.isEmpty() ? "inactive" : q.reason;
        for (const auto& order : s.orders)
            if (!order.terminal && order.buy == q.buy)
            {
                state = order.uncertain ? "uncertain" : !order.mutation.isEmpty() ? order.mutation : order.state;
                return QString("%1 @ %2 | %3 %4 | %5")
                    .arg(q.buy ? "BUY" : "SELL", displayNumber(order.price, 2),
                         displayNumber(std::max(0.0, order.total - order.filled)), s.config.base(), state);
            }
        return q.enabled ? QString("%1 @ %2 | %3 %4 | %5")
            .arg(q.buy ? "BUY" : "SELL", displayNumber(q.price, 2), displayNumber(q.amount), s.config.base(), state)
            : QString("%1: %2").arg(q.buy ? "BUY" : "SELL", state);
    };
    label(buyLevel, quoteText(s.hedgeQuotes.buy, true));
    label(sellLevel, quoteText(s.hedgeQuotes.sell, false));
    const bool valued = a.ready && a.timestamp > 0 && a.constructionId == s.constructionId;
    const bool staleValuation = !demo && (!s.connected || !s.quote.fresh(now) ||
                                          now - a.timestamp > Time::MarketDataMaxAgeMs);
    label(pnlPeriod,
          valued ? "Construction P&L (USD) | Since " + QDateTime::fromMSecsSinceEpoch(a.startedAt)
                                                           .toLocalTime()
                                                           .toString("yyyy-MM-dd HH:mm:ss 'UTC'ttt")
                 : "Construction P&L (USD) | Starts on the first successful Start");
    pnlPeriod->setToolTip("One period retained across Stop, reconnects and application restarts. "
                          "Amounts are shown in USD; USDC is valued at $1.\n" +
                          a.completeness);
    QStringList pnlNotes;
    if (valued && !a.complete)
        pnlNotes << "Incomplete";
    if (valued && staleValuation)
        pnlNotes << "Last valuation";
    label(pnlCoverage, pnlNotes.join(" | "));
    pnlCoverage->setToolTip(
        a.completeness +
        (staleValuation ? "\nShowing the last valuation; prices are not fresh." : ""));
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
            const double dollarGamma = .5 * o.quantity * o.gamma * s.quote.mid() * s.quote.mid();
            cell(positions, row, 4,
                 o.hasGreeks && validPrice ? displayNumber(dollarGamma * .0001, 2) : missing, true);
            positions->item(row, 4)->setToolTip(
                positions->horizontalHeaderItem(4)->toolTip() +
                (o.hasGreeks && validPrice
                     ? "\nDollar Gamma (0.5 x position gamma x Middle Price^2): $" +
                           displayNumber(dollarGamma, 2)
                     : QString()) +
                (!demo && (!o.freshGreeks(now) || !s.quote.fresh(now))
                     ? "\nLast available prices/Greeks; data is stale."
                     : ""));
            cell(positions, row, 5, o.hasGreeks ? displayNumber(o.quantity * o.theta, 3) : missing,
                 true);
            cell(positions, row, 6, o.hasGreeks ? displayNumber(o.quantity * o.vega, 3) : missing,
                 true);
            cell(positions, row, 7,
                 o.hasGreeks ? displayNumber(o.mark, o.spec.inverse ? 6 : 2) : missing, true);
            positions->item(row, 7)->setToolTip("Mark in " + o.spec.settlement);
            const QString dataState = std::abs(o.quantity) < Epsilon      ? "Closed"
                                      : o.spec.expiry <= utcNow()         ? "Expired"
                                      : (demo || o.freshGreeks(utcNow())) ? "Live"
                                                                          : "Stale";
            cell(positions, row, 8, dataState);
            positions->item(row, 8)->setToolTip(
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
            for (int col = 3; col <= 6; ++col)
                cell(positions, row, col, "0", true);
            cell(positions, row, 7, s.quote.mark > 0 ? displayNumber(s.quote.mark, 2) : missing,
                 true);
            cell(positions, row, 8, ready && s.quote.fresh(now) ? "Live" : "Stale");
            positions->item(row, 0)->setToolTip(
                "Actual bot perpetual hedge; delta equals signed quantity.");
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
                     : !s.problem.isEmpty()           ? s.problem
                     : s.status.startsWith("Waiting") ? s.status
                     : !s.connected && !demo          ? "Enter API credentials and click Connect."
                     : !s.synced && !demo             ? "Synchronizing positions and bot orders..."
                     : s.config.selected.isEmpty() ? "Select option positions, then Apply settings."
                     : !demo && !s.quote.fresh(now)
                         ? "Waiting for fresh bid/ask prices; trading is paused."
                     : !a.message.isEmpty() ? a.message
                     : !valued              ? "P&L begins at the first Start of this construction."
                     : !a.complete          ? "P&L history incomplete; open Details for coverage."
                     : s.running            ? "Trading active."
                                            : "Ready. Press Start / Resume to hedge.";
    label(quality, reason);
    quality->setToolTip(reason + "\n" + a.historyStatus);
    auto qualityStyle =
        !s.problem.isEmpty() && s.status == "Stopped" ? QString(theme::AlertStyle) : QString();
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
    if (pnl->isVisible())
    {
        pnl->setRowCount(1);
        const auto& v = a.pnl;
        const double nums[] = {v.options, v.realized, v.unrealized, v.optionFees + v.hedgeFees,
                               v.funding, v.net()};
        for (int col = 0; col < 6; ++col)
            cell(pnl, 0, col, valued ? displayNumber(nums[col], col == 4 ? 4 : 2) : missing, true);
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
    stopButton->setEnabled(!demo && (s.connected || s.busy));
    selectButton->setEnabled(!s.running && !s.busy);
    closeAction->setEnabled(!demo && s.synced && std::abs(s.inventory) > Epsilon && !s.busy);
    deleteAction->setEnabled(!demo && s.canDelete);
    closeAction->setToolTip(s.positionMismatch
                                ? "Exchange position differs from known fills. Reconcile orders "
                                  "and trade history before closing; profile history is retained."
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
