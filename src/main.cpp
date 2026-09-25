#include <QApplication>

#include "MainWindow.h"
#include "zrecord_version.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("zrecord");
    QApplication::setApplicationVersion(ZRECORD_VERSION);

    zrecord::MainWindow window;
    window.show();

    return app.exec();
}
