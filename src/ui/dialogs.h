#pragma once
#include <QString>
class QWidget;
namespace scalper
{
bool confirmAction(QWidget* parent, const QString& title, const QString& message,
                   bool destructive = false);
void showInformation(QWidget* parent, const QString& title, const QString& message);
} // namespace scalper
