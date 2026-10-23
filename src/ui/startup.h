#pragma once
#include "engine/domain.h"
class QApplication;

namespace scalper
{
struct StartupOptions
{
    Config config;
    QString profilePath, screenshotPath;
    bool demo = false, smokeTest = false;
};
namespace StartupTiming
{
inline constexpr int ScreenshotDelayMs = 600;
inline constexpr int SmokeTestDurationMs = 1200;
} // namespace StartupTiming
int runApplication(QApplication& application);
} // namespace scalper
