#pragma once
#include "engine/domain.h"
namespace scalper
{
struct Hedge
{
    double target = 0, delta = 0, normalized = 0;
    bool initial = false;
};
class Hedger
{
  public:
    Config config;
    Instrument spec;
    QVector<Option> options;
    Quote quote;
    Margin margin;
    double inventory = 0, referenceDelta = 0;
    bool running = false, flattening = false, anchored = false, active = false;
    qint64 completedAt = 0;
    Hedge operation;
    QString problem;
    StrategyState state = StrategyState::Paused;
    HedgeQuotes quotes;
    void plan(qint64 now);
    double reference() const;
    QString dataProblem(qint64 now, bool requireOptions = true) const;
    QString safety(qint64 now, bool requireOptions = true) const;
    bool start(qint64 now);
    void stop(const QString& reason = {});
    double target() const;
    double remaining() const;
    void complete(qint64 now);
    QJsonObject json() const;
    void restore(const QJsonObject& json);
};
} // namespace scalper
