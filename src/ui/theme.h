#pragma once
#include <QString>
class QApplication;
namespace scalper::theme
{
inline constexpr char FontFamily[] = "Segoe UI";
inline constexpr int FontSize = 9;
inline constexpr char Background[] = "#ffffff";
inline constexpr char Text[] = "#111111";
inline constexpr char Surface[] = "#ffffff";
inline constexpr char AlternateBase[] = "#f4f5f6";
inline constexpr char Button[] = "#f1f2f3";
inline constexpr char Highlight[] = "#dbeafe";
inline constexpr char HighlightedText[] = "#111111";
inline constexpr char PlaceholderText[] = "#666666";
inline constexpr char DisabledText[] = "#666666";
inline constexpr char ChartText[] = "#111111";
inline constexpr char ChartGrid[] = "#e5e5e5";
inline constexpr char ChartAxis[] = "#111111";
inline constexpr char ChartDelta[] = "#46627c";
inline constexpr char ChartZero[] = "#999999";
inline constexpr char ChartPnl[] = "#111111";
inline constexpr char ChartEscape[] = "#555555";
inline constexpr char ChartSpot[] = "#111111";
inline constexpr char Border[] = "#bdbdbd";
inline constexpr char Alert[] = "#a03030";
inline constexpr char DisabledButtonText[] = "#808080";
inline constexpr char AlertStyle[] = "color:#a03030;";
inline constexpr char NormalStyle[] = "color:#111111;";
void apply(QApplication& application);
} // namespace scalper::theme
