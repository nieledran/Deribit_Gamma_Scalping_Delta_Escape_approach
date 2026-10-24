#include "ui/action_menu.h"
#include "ui/dialogs.h"
#include "ui/format.h"
#include "ui/window.h"
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QMenu>
#include <QStackedWidget>
#include <QVBoxLayout>
namespace scalper
{
static QTableWidget* table(const QStringList& labels)
{
    auto* t = new QTableWidget;
    t->setColumnCount(labels.size());
    t->setHorizontalHeaderLabels(labels);
    t->verticalHeader()->hide();
    t->verticalHeader()->setDefaultSectionSize(24);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setAlternatingRowColors(true);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setTextElideMode(Qt::ElideRight);
    return t;
}
void Window::setupUi()
{
    resize(960, 540);
    setMinimumSize(720, 420);
    setWindowTitle("Deribit Gamma — " + initial.instrument + " — " +
                   (demo                 ? "Demo"
                    : initial.production ? "Production"
                                         : "Testnet"));
    auto* root = new QWidget;
    auto* layout = new QVBoxLayout(root);
    layout->setSpacing(5);
    setCentralWidget(root);
    auto* top = new QHBoxLayout;
    auto* instrument = new QLabel(initial.instrument);
    instrument->setObjectName("profileInstrument");
    top->addWidget(instrument);
    top->addWidget(new QLabel("|"));
    auto* environment = new QLabel(demo ? "Demo" : initial.production ? "Production" : "Testnet");
    environment->setObjectName("profileEnvironment");
    if (initial.production && !demo)
        environment->setStyleSheet(theme::AlertStyle);
    top->addWidget(environment);
    id = new QLineEdit;
    id->setObjectName("clientId");
    id->setPlaceholderText("API Client ID");
    id->setMinimumWidth(100);
    secret = new QLineEdit;
    secret->setObjectName("clientSecret");
    secret->setPlaceholderText("Secret (memory only)");
    secret->setEchoMode(QLineEdit::Password);
    secret->setMinimumWidth(120);
    top->addWidget(id, 1);
    top->addWidget(secret, 1);
    status = new QLabel("Disconnected");
    status->setObjectName("connectionStatus");
    status->setFixedWidth(fontMetrics().horizontalAdvance("Connection error") + 4);
    status->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    connectButton = new QPushButton("Connect");
    connectButton->setObjectName("connect");
    top->addWidget(connectButton);
    top->addWidget(status);
    layout->addLayout(top);
    setupDialogs();
    const int buttonWidth = std::max(100, fontMetrics().horizontalAdvance("Select options") + 20);
    const int controlHeight = std::max(connectButton->sizeHint().height(), id->sizeHint().height());
    auto alignButton = [&](QPushButton* button)
    { button->setFixedSize(buttonWidth, controlHeight); };
    alignButton(connectButton);
    id->setFixedHeight(controlHeight);
    secret->setFixedHeight(controlHeight);
    connect(connectButton, &QPushButton::clicked, this,
            [this]
            {
                if (!trading)
                    return;
                if (id->text().trimmed().isEmpty() || secret->text().isEmpty())
                {
                    showInformation(this, "Connect", "Enter the API Client ID and secret.");
                    return;
                }
                auto clientId = id->text().trimmed(), clientSecret = secret->text();
                secret->clear();
                QMetaObject::invokeMethod(trading, [w = trading, clientId, clientSecret]
                                          { w->connectExchange(clientId, clientSecret); });
            });

    const Config defaults;
    auto* settings = new QHBoxLayout;
    auto spin =
        [&](const QString& label, double maximum, int decimals, double value, const QString& name)
    {
        auto* box = new CompactSpinBox;
        box->setObjectName(name);
        box->setRange(0, maximum);
        box->setDecimals(decimals);
        box->setValue(value);
        box->setMinimumWidth(75);
        box->setFixedHeight(controlHeight);
        box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        settings->addWidget(new QLabel(label));
        settings->addWidget(box, 1);
        connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this] { markDirty(); });
        return box;
    };
    band = spin("Delta Change", Limits::MaxDeltaChange, 5, defaults.deltaChange, "deltaChange");
    band->setToolTip("Normalized option delta tolerance around confirmed hedge inventory.\n"
                     "0.50 to 0.54 = 0.04 points. Both straddle legs count in gross quantity.");
    coverage = spin("Delta coverage (%)", 1000000, 2, defaults.hedgePercent, "hedgePercent");
    coverage->setMinimum(.01);
    coverage->setToolTip(
        "Target perpetual = -option delta x coverage / 100. 200% hedges twice the delta.");
    auto* advanced = new QPushButton("Advanced");
    alignButton(advanced);
    advanced->setObjectName("advanced");
    settings->addWidget(advanced);
    layout->addLayout(settings);
    connect(advanced, &QPushButton::clicked, this,
            [this]
            {
                const auto before = configFromUi();
                const bool wasDirty = dirty;
                if (advancedDialog->exec() == QDialog::Accepted)
                    markDirty();
                else
                {
                    populateConfig(before);
                    dirty = wasDirty;
                }
            });

    auto* controls = new QHBoxLayout;
    applyButton = new ApplyButton;
    applyButton->setObjectName("applySettings");
    startButton = new QPushButton("Start / Resume");
    startButton->setObjectName("start");
    stopButton = new QPushButton("Stop & Cancel");
    stopButton->setObjectName("stop");
    selectButton = new QPushButton("Select options");
    selectButton->setObjectName("selectOptions");
    auto* graph = new QPushButton("Risk graph");
    graph->setObjectName("riskGraph");
    connect(graph, &QPushButton::clicked, this, &Window::showRisk);
    for (auto* button :
         {static_cast<QPushButton*>(applyButton), startButton, stopButton, selectButton, graph})
    {
        alignButton(button);
        controls->addWidget(button);
    }
    auto* actions = new QPushButton("Actions");
    actions->setObjectName("actions");
    alignButton(actions);
    auto* menu = new ActionMenu(actions);
    menu->setObjectName("actionsMenu");
    closeAction = menu->addAction("Close hedge");
    closeAction->setObjectName("closeHedge");
    closeAction->setProperty("danger", true);
    auto* exportAction = menu->addAction("Export CSV…");
    exportAction->setObjectName("exportCsv");
    menu->addSeparator();
    deleteAction = menu->addAction("Delete local profile");
    deleteAction->setObjectName("deleteData");
    deleteAction->setProperty("danger", true);
    actions->setMenu(menu);
    controls->addWidget(actions);
    controls->addStretch();
    layout->addLayout(controls);
    connect(selectButton, &QPushButton::clicked, this, &Window::selectOptions);
    connect(applyButton, &QPushButton::clicked, this,
            [this]
            {
                if (!trading)
                    return;
                auto config = configFromUi();
                if (auto error = config.validate(); !error.isEmpty())
                {
                    showInformation(this, "Settings", error);
                    return;
                }
                QMetaObject::invokeMethod(trading,
                                          [w = trading, config] { w->applyConfig(config); });
            });
    connect(startButton, &QPushButton::clicked, this,
            [this]
            {
                if (trading)
                    QMetaObject::invokeMethod(trading, &TradingWorker::startTrading);
            });
    connect(stopButton, &QPushButton::clicked, this,
            [this]
            {
                if (trading)
                    QMetaObject::invokeMethod(trading, &TradingWorker::stopTrading);
            });
    connect(
        closeAction, &QAction::triggered, this,
        [this]
        {
            if (trading &&
                confirmAction(
                    this, "Close bot hedge",
                    "Close only the bot's perpetual inventory using the selected execution mode?\n"
                    "Options remain open. This may increase portfolio delta and margin "
                    "requirements."))
                QMetaObject::invokeMethod(trading, &TradingWorker::closeHedge);
        });
    connect(exportAction, &QAction::triggered, this,
            [this]
            {
                if (!analytics || demo)
                    return;
                auto dir = QFileDialog::getExistingDirectory(this, "Choose CSV export directory");
                if (!dir.isEmpty())
                    QMetaObject::invokeMethod(analytics,
                                              [w = analytics, dir] { w->exportData(dir); });
            });
    connect(deleteAction, &QAction::triggered, this,
            [this]
            {
                if (trading &&
                    confirmAction(this, "Delete local profile data",
                                  "Cancel this profile's orders, then delete its local settings, "
                                  "trade history and accumulated P&L?\n"
                                  "Any exchange position remains open and unmanaged. "
                                  "A fresh empty profile will be created.\n"
                                  "Export CSV first if you need the old P&L. "
                                  "Exchange history and exports remain.",
                                  true))
                {
                    loaded = false;
                    dirty = false;
                    QMetaObject::invokeMethod(trading, &TradingWorker::deleteData);
                }
            });

    auto* marketPanel = new QFrame;
    marketPanel->setObjectName("marketPanel");
    marketPanel->setFrameShape(QFrame::StyledPanel);
    auto* quotes = new QGridLayout(marketPanel);
    quotes->setContentsMargins(8, 4, 8, 4);
    // Fixed column proportions: text changes never resize neighbouring fields.
    const int quoteWidths[] = {180, 120, 120, 200, 220};
    for (int col = 0; col < 5; ++col)
        quotes->setColumnStretch(col, quoteWidths[col]);
    rebalanceBand = new StableLabel;
    rebalanceBand->setObjectName("rebalanceBand");
    quotes->addWidget(rebalanceBand, 0, 0);
    risk = new StableLabel;
    risk->setObjectName("riskSummary");
    quotes->addWidget(risk, 0, 3);
    bidQuote = new QuoteLabel("Bid", QColor("#319263"));
    bidQuote->setObjectName("bidQuote");
    askQuote = new QuoteLabel("Ask", QColor("#c64c55"));
    askQuote->setObjectName("askQuote");
    quotes->addWidget(bidQuote, 0, 1);
    quotes->addWidget(askQuote, 0, 2);
    layout->addWidget(marketPanel);
    auto* metrics = new QHBoxLayout;
    metrics->setSpacing(3);
    auto group = [&](const char* name)
    {
        auto* panel = new QFrame;
        panel->setFrameShape(QFrame::StyledPanel);
        panel->setObjectName(name);
        panel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto* fields = new QGridLayout(panel);
        fields->setContentsMargins(8, 4, 8, 4);
        fields->setVerticalSpacing(0);
        fields->setColumnStretch(0, 1);
        metrics->addWidget(panel, 1);
        return fields;
    };
    auto* deltaFields = group("deltaPanel");
    auto* hedgeFields = group("hedgePanel");
    auto* executionFields = group("executionPanel");
    auto metric = [&](QGridLayout* fields, int row, const char* name)
    {
        auto* value = new StableLabel;
        value->setObjectName(name);
        fields->addWidget(value, row, 0);
        return value;
    };
    netDeltaValue = metric(deltaFields, 0, "netDeltaValue");
    normalizedValue = metric(deltaFields, 1, "normalizedDelta");
    referenceValue = metric(deltaFields, 2, "referenceDelta");
    targetDeltaValue = metric(deltaFields, 3, "targetNetDelta");
    hedgeValue = metric(hedgeFields, 0, "hedgeValue");
    hedgeUsage = metric(hedgeFields, 1, "hedgeUsage");
    hedgeRemaining = metric(hedgeFields, 2, "hedgeRemaining");
    legsValue = metric(hedgeFields, 3, "legsValue");
    auto* stateRow = new QGridLayout;
    stateRow->setColumnStretch(0, 2);
    stateRow->setColumnStretch(1, 3);
    estimate = new StableLabel;
    estimate->setObjectName("hedgeEstimate");
    executionState = new StableLabel;
    executionState->setObjectName("executionState");
    stateRow->addWidget(estimate, 0, 0);
    stateRow->addWidget(executionState, 0, 1);
    quotes->addLayout(stateRow, 0, 4);
    activity = metric(executionFields, 0, "tradingActivity");
    breakEvenMove = metric(executionFields, 1, "breakEvenMove");
    buyLevel = metric(executionFields, 2, "buyHedgeLevel");
    sellLevel = metric(executionFields, 3, "sellHedgeLevel");
    layout->addLayout(metrics);
    tabs = new QTabWidget;
    tabs->setObjectName("mainTabs");
    positions = table(
        {"Instrument", "Qty", "Delta", "Gamma", "Gamma P&L (1%)", "Theta", "Vega", "Mark", "Data"});
    positions->setObjectName("optionPositions");
    positions->horizontalHeader()->setStretchLastSection(false);
    positions->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    const int widths[] = {0, 65, 80, 85, 120, 65, 65, 75, 50};
    for (int col = 1; col < 9; ++col)
        positions->setColumnWidth(col, widths[col]);
    positions->horizontalHeaderItem(2)->setToolTip("Position delta (coin)");
    positions->horizontalHeaderItem(3)->setToolTip("Position gamma (coin per USD)");
    positions->horizontalHeaderItem(4)->setToolTip(
        "Approximate gamma-only P&L in USD for a 1% price move in either direction: "
        "0.5 x position gamma x (0.01 x Middle Price)^2. Signed quantity included. "
        "Constant gamma; excludes delta, theta, IV changes and trading costs. "
        "The original Dollar Gamma coefficient is available in each cell's tooltip.");
    positions->horizontalHeaderItem(5)->setToolTip(
        "Position theta (USD, API remaining-lifetime cap)");
    positions->horizontalHeaderItem(6)->setToolTip("Position vega (USD per volatility point)");
    positions->setToolTip("Double-click a leg for Greeks and contract details.");
    tabs->addTab(positions, "Construction");
    orders = table({"Side", "Price", "Amount", "Filled", "Execution", "State"});
    orders->setObjectName("hedgeOrders");
    for (int i = 0; i < 6; ++i)
        orders->setColumnWidth(i, i == 4 ? 140 : 105);
    tabs->addTab(orders, "Orders");
    journal = new QPlainTextEdit;
    journal->setObjectName("journal");
    journal->setReadOnly(true);
    journal->setMaximumBlockCount(Limits::JournalMessages);
    tabs->addTab(journal, "Journal");
    layout->addWidget(tabs, 1);
    connect(tabs, &QTabWidget::currentChanged, this, [this] { render(); });
    connect(positions, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { showLeg(row); });
    auto* pnlPanel = new QFrame;
    pnlPanel->setObjectName("pnlPanel");
    pnlPanel->setFrameShape(QFrame::StyledPanel);
    auto* pnlLayout = new QVBoxLayout(pnlPanel);
    pnlLayout->setContentsMargins(6, 4, 6, 4);
    pnlLayout->setSpacing(3);
    pnlPeriod = new StableLabel;
    pnlPeriod->setObjectName("pnlPeriod");
    auto* pnlCaption = new QHBoxLayout;
    pnlCaption->addWidget(pnlPeriod, 4);
    pnlCoverage = new StableLabel;
    pnlCoverage->setObjectName("pnlCoverage");
    pnlCaption->addWidget(pnlCoverage, 1);
    pnlLayout->addLayout(pnlCaption);
    pnl = table({"Options", "Futures closed", "Futures open", "Fees", "Funding", "Total P&L"});
    pnl->setObjectName("pnlSummary");
    pnl->setRowCount(1);
    pnl->setSelectionMode(QAbstractItemView::NoSelection);
    pnl->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    pnl->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pnl->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pnl->setFixedHeight(pnl->horizontalHeader()->sizeHint().height() +
                        pnl->verticalHeader()->defaultSectionSize() + 4);
    pnl->horizontalHeaderItem(2)->setToolTip(
        "Change in open futures P&L since this construction's first Start.");
    pnl->horizontalHeaderItem(3)->setToolTip(
        "Option and futures commissions since first Start; deducted from total P&L.");
    pnlLayout->addWidget(pnl);
    layout->addWidget(pnlPanel);
    auto* footer = new QHBoxLayout;
    auto* connectionPanel = new QFrame;
    connectionPanel->setObjectName("connectionPanel");
    connectionPanel->setFrameShape(QFrame::StyledPanel);
    auto* connection = new QGridLayout(connectionPanel);
    connection->setContentsMargins(8, 4, 8, 4);
    const int footerWidths[] = {110, 8, 125, 8, 240};
    for (int col = 0; col < 5; ++col)
        connection->setColumnStretch(col, footerWidths[col]);
    latency = new StableLabel;
    latency->setText("Latency: — ms");
    latency->setObjectName("latencyStatus");
    latency->setToolTip("Exchange request round-trip time; separate from market-data age.");
    connection->addWidget(latency, 0, 0);
    connection->addWidget(new QLabel("|"), 0, 1);
    dataAge = new StableLabel;
    dataAge->setText("Data age: — ms");
    dataAge->setObjectName("quoteAge");
    dataAge->setToolTip(
        "Age of the latest perpetual quote, adjusted for the measured exchange clock difference.");
    connection->addWidget(dataAge, 0, 2);
    connection->addWidget(new QLabel("|"), 0, 3);
    clockDifference = new StableLabel;
    clockDifference->setObjectName("clockDifference");
    clockDifference->setToolTip("Exchange clock versus PC, estimated by public/get_time. This is a "
                                "clock difference, not network latency.");
    connection->addWidget(clockDifference, 0, 4);
    footer->addWidget(connectionPanel, 3);
    auto* messagePanel = new QFrame;
    messagePanel->setObjectName("messagePanel");
    messagePanel->setFrameShape(QFrame::StyledPanel);
    auto* message = new QHBoxLayout(messagePanel);
    message->setContentsMargins(8, 4, 8, 4);
    quality = new StableLabel;
    quality->setObjectName("dataQuality");
    quality->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    message->addWidget(quality);
    footer->addWidget(messagePanel, 2);
    layout->addLayout(footer);
}
void Window::setupDialogs()
{
    advancedDialog = new QDialog(this);
    advancedDialog->setObjectName("advancedDialog");
    advancedDialog->setWindowTitle("Advanced settings");
    advancedDialog->setMinimumWidth(450);
    auto* advanced = new QFormLayout(advancedDialog);
    advanced->setSizeConstraint(QLayout::SetFixedSize);
    auto add = [&](const QString& title, double min, double max, int decimals, const QString& name)
    {
        auto* box = new CompactSpinBox;
        box->setObjectName(name);
        box->setRange(min, max);
        box->setDecimals(decimals);
        advanced->addRow(title, box);
        return box;
    };
    auto* policy = new QLabel("Long-gamma options strategy | Post-only GTC limits");
    policy->setWordWrap(true);
    policy->setObjectName("executionPolicy");
    advanced->addRow(policy);
    tradingSpacing = new QSpinBox;
    tradingSpacing->setObjectName("tradingRequestSpacing");
    tradingSpacing->setRange(Exchange::MinimumTradingIntervalMs, Limits::MaxTradingIntervalMs);
    tradingSpacing->setToolTip("Minimum time between trading requests. Deribit's account rate "
                               "limit can require a longer interval.");
    advanced->addRow("Trading-request spacing (ms):", tradingSpacing);
    connect(tradingSpacing, qOverload<int>(&QSpinBox::valueChanged), this, [this] { markDirty(); });
    fallbackMaker = add("Fallback maker fee (%):", -100, 100, 4, "fallbackMakerFee");
    fallbackTaker = add("Fallback taker fee (%):", 0, 100, 4, "fallbackTakerFee");
    fallbackMaker->setToolTip("Maker commission estimate when account fee rates are unavailable. "
                              "Negative values represent rebates. Account rates take precedence.");
    fallbackTaker->setToolTip("Taker commission estimate when account fee rates are unavailable. "
                              "Account rates take precedence.");
    marginEnabled = new QCheckBox("Stop at account MM usage");
    marginEnabled->setObjectName("marginLimitEnabled");
    advanced->addRow(marginEnabled);
    margin = add("MM / Margin Balance (%):", .01, 99.99, 2, "maxMarginUsage");
    marginInfo = new QLabel("Account margin data unavailable");
    marginInfo->setWordWrap(true);
    const auto margins = advanced->contentsMargins();
    marginInfo->setMinimumWidth(advancedDialog->minimumWidth() - margins.left() - margins.right());
    advanced->addRow(marginInfo);
    auto* done = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    advanced->addRow(done);
    connect(done, &QDialogButtonBox::accepted, advancedDialog, &QDialog::accept);
    connect(done, &QDialogButtonBox::rejected, advancedDialog, &QDialog::reject);
    connect(marginEnabled, &QCheckBox::toggled, margin, &QWidget::setEnabled);
}
} // namespace scalper
