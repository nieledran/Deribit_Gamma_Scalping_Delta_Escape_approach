#include "ui/format.h"
#include "ui/window.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QTimeZone>
#include <QVBoxLayout>
namespace scalper
{
void Window::selectOptions()
{
    TradingSnapshot s;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        s = shared->trading;
    }
    QDialog dialog(this);
    dialog.setObjectName("optionSelection");
    dialog.setWindowTitle("Select existing option positions");
    dialog.resize(790, 420);
    auto* layout = new QVBoxLayout(&dialog);
    const bool locked = s.running || s.busy || std::abs(s.inventory) > Epsilon;
    auto* note = new QLabel(
        locked ? "Close the bot hedge and stop trading before changing the option basket."
               : "Select existing positions. Save the selection with Apply in the main window.");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* filters = new QHBoxLayout;
    auto* expiry = new QComboBox;
    expiry->setObjectName("expiryFilter");
    expiry->addItem("All expiries", qint64(0));
    auto* type = new QComboBox;
    type->setObjectName("optionTypeFilter");
    type->addItems({"Call + Put", "Call", "Put"});
    filters->addWidget(new QLabel("Expiry:"));
    filters->addWidget(expiry, 1);
    filters->addWidget(new QLabel("Type:"));
    filters->addWidget(type);
    layout->addLayout(filters);
    auto* table = new QTableWidget;
    table->setObjectName("optionSelectionTable");
    table->setColumnCount(6);
    table->setHorizontalHeaderLabels(
        {"Select", "Option instrument", "Qty", "Delta", "Expiry UTC", "Data"});
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setColumnWidth(0, 48);
    table->setColumnWidth(1, 280);
    table->setColumnWidth(2, 80);
    table->setColumnWidth(3, 90);
    table->setColumnWidth(4, 105);
    table->horizontalHeader()->setStretchLastSection(true);
    auto open = s.openOptions(utcNow());
    table->setRowCount(open.size());
    QSet<qint64> expiries;
    for (int row = 0; row < open.size(); ++row)
    {
        const auto& o = open[row];
        auto* check = new QTableWidgetItem;
        check->setFlags(Qt::ItemIsUserCheckable | (locked ? Qt::NoItemFlags : Qt::ItemIsEnabled));
        check->setCheckState(draftSelected.contains(o.spec.name) ? Qt::Checked : Qt::Unchecked);
        table->setItem(row, 0, check);
        const QStringList columns = {o.spec.name, displayNumber(o.quantity),
                                     displayNumber(o.quantity * o.delta),
                                     QDateTime::fromMSecsSinceEpoch(o.spec.expiry, QTimeZone::utc())
                                         .toString("dd MMM HH:mm"),
                                     demo || o.freshGreeks(utcNow()) ? "Live" : "Stale"};
        for (int col = 1; col < 6; ++col)
        {
            auto* item = new QTableWidgetItem(columns[col - 1]);
            if (col == 2 || col == 3)
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            table->setItem(row, col, item);
        }
        table->item(row, 4)->setToolTip(isoUtc(o.spec.expiry));
        expiries.insert(o.spec.expiry);
    }
    auto dates = expiries.values();
    std::sort(dates.begin(), dates.end());
    for (auto date : dates)
        expiry->addItem(
            QDateTime::fromMSecsSinceEpoch(date, QTimeZone::utc()).toString("dd MMM yyyy HH:mm"),
            date);
    layout->addWidget(table, 1);
    auto* summary = new QLabel;
    layout->addWidget(summary);
    auto update = [&]
    {
        QVector<Option> checked;
        for (int row = 0; row < open.size(); ++row)
        {
            if (table->item(row, 0)->checkState() == Qt::Checked)
                checked.append(open[row]);
            table->setRowHidden(row,
                                (expiry->currentData().toLongLong() != 0 &&
                                 expiry->currentData().toLongLong() != open[row].spec.expiry) ||
                                    (type->currentIndex() == 1 && !open[row].isCall) ||
                                    (type->currentIndex() == 2 && open[row].isCall));
        }
        summary->setText(QString("%1 legs selected | Option delta %2 %3")
                             .arg(checked.size())
                             .arg(displayNumber(aggregate(checked).delta), s.config.base()));
    };
    connect(expiry, &QComboBox::currentIndexChanged, &dialog, update);
    connect(type, &QComboBox::currentIndexChanged, &dialog, update);
    connect(table, &QTableWidget::itemChanged, &dialog, [&](QTableWidgetItem*) { update(); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Apply)->setEnabled(!locked);
    layout->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog,
            &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    update();
    if (dialog.exec() == QDialog::Accepted)
    {
        QStringList names;
        for (int row = 0; row < open.size(); ++row)
            if (table->item(row, 0)->checkState() == Qt::Checked)
                names.append(open[row].spec.name);
        if (names != draftSelected)
        {
            draftSelected = names;
            markDirty();
            render();
        }
    }
}
void Window::showRisk()
{
    if (riskDialog)
    {
        riskDialog->showNormal();
        riskDialog->raise();
        riskDialog->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    riskDialog = dialog;
    dialog->setObjectName("riskDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Construction risk — snapshot");
    dialog->resize(920, 560);
    auto* layout = new QVBoxLayout(dialog);
    auto* header = new QHBoxLayout;
    riskTimestamp = new QLabel("Preparing snapshot…");
    riskTimestamp->setObjectName("riskTimestamp");
    riskTimestamp->setWordWrap(true);
    header->addWidget(riskTimestamp, 1);
    auto* greek = new QComboBox;
    greek->setObjectName("scenarioGreek");
    greek->addItems({"Delta", "Gamma", "Theta", "Vega"});
    header->addWidget(greek);
    riskRefresh = new QPushButton("Refresh");
    riskRefresh->setObjectName("riskRefresh");
    header->addWidget(riskRefresh);
    layout->addLayout(header);
    riskIv = new QLabel("IV: preparing snapshot");
    riskIv->setObjectName("riskIv");
    riskIv->setWordWrap(true);
    layout->addWidget(riskIv);
    chart = new ProfileChart;
    layout->addWidget(chart, 1);
    auto* note = new QLabel(
        "Snapshot only: time and each option's IV are fixed. Includes the current hedge; "
        "no future hedge trades.");
    note->setWordWrap(true);
    layout->addWidget(note);
    connect(greek, &QComboBox::currentIndexChanged, chart,
            [this](int index) { chart->setGreek(ScenarioGreek(index)); });
    connect(riskRefresh, &QPushButton::clicked, this, &Window::requestRisk);
    connect(dialog, &QObject::destroyed, this,
            [this]
            {
                chart = nullptr;
                riskTimestamp = nullptr;
                riskIv = nullptr;
                riskRefresh = nullptr;
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->activeRiskRequest = ++riskRequestId;
            });
    dialog->show();
    requestRisk();
}
void Window::requestRisk()
{
    if (!riskDialog || !analytics)
        return;
    RiskRequest request;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        request.id = ++riskRequestId;
        request.trading = shared->trading;
        request.demo = demo;
        request.demoPnl = shared->analytics.pnl;
        shared->activeRiskRequest = request.id;
    }
    riskRefresh->setEnabled(false);
    riskTimestamp->setText("Calculating snapshot…");
    QMetaObject::invokeMethod(analytics, [w = analytics, request] { w->requestRisk(request); });
}
void Window::showLeg(int row)
{
    if (!positions->item(row, 0))
        return;
    const auto name = positions->item(row, 0)->text();
    TradingSnapshot s;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        s = shared->trading;
    }
    if (name == s.config.instrument)
    {
        auto* dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(name);
        auto* form = new QFormLayout(dialog);
        form->addRow("Quantity / delta:",
                     new QLabel(displayNumber(s.inventory) + " " + s.config.base()));
        form->addRow("Target:", new QLabel(displayNumber(s.target) + " " + s.config.base()));
        form->addRow("Notional:",
                     new QLabel("$" + displayNumber(std::abs(s.inventory) * s.quote.mid(), 2)));
        form->addRow("Mark:", new QLabel(displayNumber(s.quote.mark, 2) + " USDC"));
        dialog->show();
        return;
    }
    for (const auto& o : s.positions)
        if (o.spec.name == name)
        {
            auto* dialog = new QDialog(this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle(name);
            auto* form = new QFormLayout(dialog);
            const auto values =
                QStringList{displayNumber(o.quantity) + " " + s.config.base(),
                            displayNumber(o.quantity * o.delta) + " " + s.config.base(),
                            displayNumber(o.quantity * o.gamma, 9) + " " + s.config.base() + "/$",
                            displayNumber(o.quantity * o.theta, 3) + " USD (API)",
                            displayNumber(o.quantity * o.vega, 3) + " USD / vol point",
                            displayNumber(o.iv * 100, 2) + "%",
                            isoUtc(o.spec.expiry),
                            o.spec.settlement};
            const QStringList labels = {"Quantity:", "Delta:", "Gamma:",      "Theta:",
                                        "Vega:",     "IV:",    "Expiry UTC:", "Settlement:"};
            for (int i = 0; i < labels.size(); ++i)
                form->addRow(labels[i], new QLabel(values[i]));
            dialog->show();
            return;
        }
}
} // namespace scalper
