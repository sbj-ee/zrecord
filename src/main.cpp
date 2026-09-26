#include <QApplication>

#include "MainWindow.h"
#include "zrecord_version.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("zrecord");
    // QSettings (recent projects, window geometry) lives in
    // ~/.config/zrecord/zrecord.conf.
    QApplication::setOrganizationName("zrecord");
    QApplication::setApplicationVersion(ZRECORD_VERSION);

    zrecord::MainWindow window;
    window.show();

    return app.exec();
}
