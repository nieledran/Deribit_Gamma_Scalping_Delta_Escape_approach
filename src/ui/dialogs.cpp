#include "ui/dialogs.h"
#include <QMessageBox>
#include <QPushButton>

namespace scalper
{
bool confirmAction(QWidget* parent, const QString& title, const QString& message, bool destructive)
{
    QMessageBox dialog(QMessageBox::NoIcon, title, message, QMessageBox::Yes | QMessageBox::No,
                       parent);
    dialog.setObjectName(destructive ? "deleteConfirmation" : "actionConfirmation");
    dialog.setDefaultButton(QMessageBox::No);
    dialog.setEscapeButton(QMessageBox::No);
    if (destructive)
    {
        dialog.button(QMessageBox::Yes)->setProperty("danger", true);
        auto palette = dialog.button(QMessageBox::Yes)->palette();
        palette.setColor(QPalette::ButtonText, QColor("#a03030"));
        dialog.button(QMessageBox::Yes)->setPalette(palette);
    }
    return dialog.exec() == QMessageBox::Yes;
}
void showInformation(QWidget* parent, const QString& title, const QString& message)
{
    QMessageBox dialog(QMessageBox::NoIcon, title, message, QMessageBox::Ok, parent);
    dialog.exec();
}
} // namespace scalper
