#include "ui/theme.h"
#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
namespace scalper::theme
{
void apply(QApplication& application)
{
    application.setStyle(QStyleFactory::create("Fusion"));
    QPalette palette = application.style()->standardPalette();
    palette.setColor(QPalette::Window, Qt::white);
    palette.setColor(QPalette::WindowText, Qt::black);
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::AlternateBase, QColor("#f4f5f6"));
    palette.setColor(QPalette::Text, Qt::black);
    palette.setColor(QPalette::Button, QColor("#f1f2f3"));
    palette.setColor(QPalette::ButtonText, Qt::black);
    palette.setColor(QPalette::PlaceholderText, QColor("#6b7280"));
    palette.setColor(QPalette::Highlight, QColor("#dbeafe"));
    palette.setColor(QPalette::HighlightedText, Qt::black);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#808080"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#808080"));
    application.setPalette(palette);
    application.setFont(QFont(FontFamily, FontSize));
    application.setStyleSheet({});
}
} // namespace scalper::theme
