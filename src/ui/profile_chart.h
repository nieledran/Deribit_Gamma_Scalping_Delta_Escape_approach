#pragma once
#include "engine/domain.h"
#include <QWidget>
#include <array>
namespace scalper
{
class ProfileChart : public QWidget
{
    Q_OBJECT
  public:
    explicit ProfileChart(QWidget* parent = nullptr);
    void setData(const RiskResult& result);
    void setGreek(ScenarioGreek value);

  protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

  private:
    RiskResult result;
    ScenarioGreek greek = ScenarioGreek::Delta;
    double pnlStep = 0;
    std::array<double, 4> greekLimits{};
    double greekValue(const ProfilePoint&) const;
    QString greekLabel() const;
    double minimumPrice = 0, maximumPrice = 1, minimumPnl = 0, maximumPnl = 0, minimumGreek = -1,
           maximumGreek = 1;
    QRectF plot;
};
} // namespace scalper
