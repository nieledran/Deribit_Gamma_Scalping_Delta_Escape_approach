#include "config/build_info.h"
#include "ui/startup.h"
#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(scalper::BuildInfo::Name);
    application.setApplicationVersion(scalper::BuildInfo::Version);
    return scalper::runApplication(application);
}
