#include <QApplication>
#include <QTimer>

#include "MainWindow.h"
#include "zrecord_version.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    // Before QApplication, so it works without a display (e.g. to check an
    // installed package).
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("zrecord %s\n", ZRECORD_VERSION);
            return 0;
        }
    }
    QApplication app(argc, argv);
    QApplication::setApplicationName("zrecord");
    // QSettings (recent projects, window geometry) lives in
    // ~/.config/zrecord/zrecord.conf.
    QApplication::setOrganizationName("zrecord");
    QApplication::setApplicationVersion(ZRECORD_VERSION);

    zrecord::MainWindow window;
    window.show();
    // Once the window is up: offer to restore work a crash left behind.
    QTimer::singleShot(0, &window, &zrecord::MainWindow::offerRecovery);

    return app.exec();
}
