#include "ui/startup.h"
#include "ui/theme.h"
#include "ui/window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLocale>
#include <QMessageBox>
#include <QTimer>

namespace scalper
{
namespace
{
bool selectProfile(Config& config, const QString& previewPath = {}, bool preview = false)
{
    QDialog dialog;
    dialog.setWindowTitle("Deribit Gamma — Settings");
    dialog.setMinimumWidth(420);
    auto* layout = new QFormLayout(&dialog);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    QComboBox instrument;
    instrument.addItems(Exchange::hedgeInstruments());
    QComboBox environment;
    environment.addItems({"Testnet", "Production"});
    layout->addRow("Instrument:", &instrument);
    layout->addRow("Environment:", &environment);
    auto* note = new QLabel("Each instrument and environment has its own saved settings.\n"
                            "To run another instrument, launch the program again.");
    note->setWordWrap(false);
    note->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addRow(note);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons.button(QDialogButtonBox::Ok)->setText("Open");
    layout->addRow(&buttons);
    QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (preview)
    {
        if (!previewPath.isEmpty())
            QTimer::singleShot(150, &dialog, [&] { dialog.grab().save(previewPath); });
        QTimer::singleShot(1000, &dialog, &QDialog::reject);
    }
    if (dialog.exec() != QDialog::Accepted)
        return false;
    config.instrument = instrument.currentText();
    config.production = environment.currentIndex() == 1;
    return true;
}

std::optional<StartupOptions> parseOptions(QApplication& application)
{
    StartupOptions options;
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(
        {"profile", "Gamma SQLite profile path (grid profiles are incompatible)", "path"});
    parser.addOption(
        {"instrument", "Hedge perpetual for a new profile", "name", options.config.instrument});
    parser.addOption({"production", "Production for a new profile"});
    parser.addOption(
        {"demo", "Offline synthetic UI demonstration; no exchange connection or profile writes"});
    parser.addOption({"smoke-test", "Open without connecting and exit after one second"});
    parser.addOption({"screenshot", "Save an offline UI screenshot", "path"});
    parser.addOption(
        {"preview-profile", "Preview the startup dialog for one second without opening a profile"});
    parser.process(application);
    options.config.instrument = parser.value("instrument");
    options.config.production = parser.isSet("production");
    options.profilePath = parser.value("profile");
    options.screenshotPath = parser.value("screenshot");
    options.demo = parser.isSet("demo");
    options.smokeTest = parser.isSet("smoke-test");
    if (parser.isSet("preview-profile"))
    {
        selectProfile(options.config, options.screenshotPath, true);
        return std::nullopt;
    }
    const bool needsSelection = !options.demo && options.profilePath.isEmpty() &&
                                !parser.isSet("instrument") && !options.smokeTest;
    if (needsSelection && !selectProfile(options.config))
        return std::nullopt;
    return options;
}
} // namespace

int runApplication(QApplication& application)
{
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedKingdom));
    theme::apply(application);
    auto parsed = parseOptions(application);
    if (!parsed)
        return 0;
    auto options = std::move(*parsed);
    if (!Exchange::supportedHedge(options.config.instrument))
    {
        QMessageBox::critical(nullptr, "Invalid instrument",
                              "Choose " + Exchange::hedgeInstruments().join(" or "));
        return 1;
    }
    if (options.profilePath.isEmpty())
        options.profilePath = QCoreApplication::applicationDirPath() + "/gamma-profiles/" +
                              options.config.base().toLower() +
                              (options.config.production ? "-production.db" : "-testnet.db");
    try
    {
        Window window(QFileInfo(options.profilePath).absoluteFilePath(), options.config,
                      options.demo);
        window.show();
        if (!options.screenshotPath.isEmpty())
            QTimer::singleShot(StartupTiming::ScreenshotDelayMs, &window,
                               [&window, path = options.screenshotPath]
                               { window.grab().save(path); });
        if (options.smokeTest)
            QTimer::singleShot(StartupTiming::SmokeTestDurationMs, &window, &QWidget::close);
        return application.exec();
    }
    catch (const std::exception& error)
    {
        QMessageBox::critical(nullptr, "Cannot open gamma profile",
                              QString::fromUtf8(error.what()));
        return 1;
    }
}
} // namespace scalper
