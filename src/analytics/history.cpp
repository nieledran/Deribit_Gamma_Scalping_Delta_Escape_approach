#include "analytics/worker.h"
#include <QSqlQuery>
#include <algorithm>
namespace scalper
{
void AnalyticsWorker::connectHistory(QString id, QString secret, bool production, QString identity,
                                     QUrl endpoint)
{
    guard(
        [&]
        {
            ++epoch;
            ready = false;
            busy = false;
            verifying = historyBlocked = false;
            historyRetryAt = 0;
            historyRetryMs = Time::SecondMs;
            account = identity;
            imports.clear();
            scheduled.clear();
            lastFundingRefresh = 0;
            historyStatus = "Connecting read-only history stream";
            history->connectTo(id, secret, production, endpoint);
        });
}
void AnalyticsWorker::disconnectHistory()
{
    ++epoch;
    ready = false;
    busy = false;
    verifying = false;
    imports.clear();
    if (history)
        history->stop();
}
void AnalyticsWorker::authenticateHistory()
{
    if (verifying || historyBlocked || !history->authenticated())
        return;
    verifying = true;
    const int generation = epoch;
    history->call("private/get_account_summary", {{"currency", "USDC"}, {"extended", true}},
                  [this, generation](const RpcResult& result)
                  { guard([&] { verifyHistoryAccount(generation, result); }, true); });
}
void AnalyticsWorker::verifyHistoryAccount(int generation, const RpcResult& result)
{
    if (generation != epoch)
        return;
    verifying = false;
    if (!result.ok)
    {
        retryHistory("History account verification failed: " + result.error);
        return;
    }
    const auto s = snapshot();
    const auto value = result.value.toObject()["id"];
    const QString id = value.isString() ? value.toString() : QString::number(value.toInteger());
    if (id.isEmpty() || id == "0")
        throw std::runtime_error("History account identity is unavailable");
    const QString identity =
        id + "/" + (s.config.production ? "production/" : "testnet/") + s.config.instrument;
    if (identity != account)
    {
        historyBlocked = true;
        history->stop();
        throw std::runtime_error("History connection account does not match the trading profile");
    }
    ready = true;
    historyRetryAt = 0;
    historyRetryMs = Time::SecondMs;
    historyStatus = "History connected";
    scheduleImports(s);
}
void AnalyticsWorker::retryHistory(const QString& reason)
{
    emit reportIssue(reason);
    busy = verifying = false;
    historyStatus = reason + "; retrying automatically";
    historyRetryAt = utcNow() + historyRetryMs;
    historyRetryMs = std::min(Exchange::MaxRetryMs, historyRetryMs * 2);
}
void AnalyticsWorker::scheduleImports(const TradingSnapshot& s)
{
    if (!ledger || !ready)
        return;
    for (const auto& option : s.positions)
    {
        if (!s.config.selected.contains(option.spec.name))
            continue;
        queueOptionImport(s, option);
        queueDeliveryImport(s, option);
    }
    queueFundingImport(s);
}
void AnalyticsWorker::queueOptionImport(const TradingSnapshot& s, const Option& option)
{
    const QString key = s.constructionId + "/" + option.spec.name;
    const qint64 previous = ledger->metadata("option_cursor/" + key)["at"].toInteger();
    if (scheduled.contains(key) ||
        (previous && s.timestamp - previous < History::RefreshIntervalMs))
        return;
    scheduled.insert(key);
    Import job;
    job.kind = ImportKind::Option;
    job.name = option.spec.name;
    job.currency = option.spec.settlement;
    job.construction = s.constructionId;
    job.option = option;
    job.start = previous ? std::max<qint64>(1, previous - History::OverlapMs)
                         : std::max<qint64>(1, option.spec.creation);
    job.end = s.timestamp;
    imports.enqueue(job);
}
void AnalyticsWorker::queueDeliveryImport(const TradingSnapshot& s, const Option& option)
{
    const QString key = s.constructionId + "/" + option.spec.name;
    if (option.spec.expiry <= 0 || s.timestamp < option.spec.expiry ||
        scheduled.contains(key + "/delivery"))
        return;
    const qint64 previous = ledger->metadata("delivery_cursor/" + key)["at"].toInteger();
    if (s.timestamp - previous < History::RefreshIntervalMs)
        return;
    scheduled.insert(key + "/delivery");
    Import job;
    job.kind = ImportKind::Delivery;
    job.name = option.spec.name.section('-', 0, 1);
    job.currency = option.spec.settlement;
    job.construction = s.constructionId;
    job.deliveryKey = key;
    job.start = option.spec.expiry - History::DeliveryWindowMs;
    job.end = s.timestamp;
    imports.enqueue(job);
}
void AnalyticsWorker::queueFundingImport(const TradingSnapshot& s)
{
    const QString key = s.constructionId + "/funding";
    if (scheduled.contains(key) || s.timestamp - lastFundingRefresh < History::RefreshIntervalMs)
        return;
    lastFundingRefresh = s.timestamp;
    Import job;
    job.kind = ImportKind::Perpetual;
    job.name = s.config.instrument;
    job.currency = "USDC";
    job.construction = s.constructionId;
    job.end = s.timestamp;
    job.start = ledger->metadata("perp_log_cursor")["at"].toInteger();
    if (!job.start)
    {
        QSqlQuery query(ledger->database());
        query.prepare("SELECT MIN(at) FROM trades WHERE construction=?");
        query.addBindValue(s.constructionId);
        if (query.exec() && query.next())
            job.start = query.value(0).toLongLong();
    }
    if (job.start <= 0)
        return;
    job.start = std::max<qint64>(1, job.start - History::OverlapMs);
    scheduled.insert(key);
    imports.enqueue(job);
}
void AnalyticsWorker::nextImport()
{
    if (historyBlocked || utcNow() < historyRetryAt || !history->authenticated())
        return;
    if (!ready)
    {
        authenticateHistory();
        return;
    }
    if (busy || imports.isEmpty() || !ledger)
        return;
    const Import job = imports.head();
    QJsonObject params{{"currency", job.currency},
                       {"start_timestamp", job.start},
                       {"end_timestamp", job.end},
                       {"query", job.name},
                       {"count", History::PageSize}};
    if (!job.cursor.isUndefined() && !job.cursor.isNull())
        params["continuation"] = job.cursor;
    busy = true;
    const int generation = epoch;
    history->call("private/get_transaction_log", params,
                  [this, generation, job](const RpcResult& result)
                  { guard([&] { receiveImport(generation, job, result); }, true); });
}
void AnalyticsWorker::receiveImport(int generation, const Import& job, const RpcResult& response)
{
    if (generation != epoch)
        return;
    if (!response.ok)
    {
        retryHistory("History import delayed: " + response.error);
        return;
    }
    const auto result = response.value.toObject();
    if (!result["logs"].isArray())
        throw std::runtime_error("Invalid transaction history response");
    QJsonArray relevant;
    for (const auto& value : result["logs"].toArray())
        if (value.toObject()["instrument_name"].toString() == job.name)
            relevant.append(value);
    ledger->logEntries(job.currency, relevant);
    const auto cursor = result["continuation"];
    if (!cursor.isNull() && !cursor.isUndefined())
    {
        if (cursor == job.cursor)
            throw std::runtime_error("History pagination did not advance");
        imports.head().cursor = cursor;
    }
    else
    {
        finishImport(job);
        imports.dequeue();
    }
    historyRetryAt = 0;
    historyRetryMs = Time::SecondMs;
    busy = false;
    historyStatus = QString("History synchronized: %1; %2 request(s) pending")
                        .arg(job.name)
                        .arg(imports.size());
}
void AnalyticsWorker::finishImport(const Import& job)
{
    switch (job.kind)
    {
    case ImportKind::Option:
    {
        ledger->finishImport(job.construction, job.option);
        const QString key = job.construction + "/" + job.name;
        ledger->metadata("option_cursor/" + key, {{"at", job.end}});
        scheduled.remove(key);
        return;
    }
    case ImportKind::Perpetual:
        ledger->metadata("perp_log_cursor", {{"at", job.end}});
        scheduled.remove(job.construction + "/funding");
        return;
    case ImportKind::Delivery:
        ledger->metadata("delivery_cursor/" + job.deliveryKey, {{"at", job.end}});
        scheduled.remove(job.deliveryKey + "/delivery");
        return;
    }
}
} // namespace scalper
