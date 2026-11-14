// SPDX-License-Identifier: Apache-2.0
#include "window.h"
#include "companion.h"
#include <QApplication>
#include <QDir>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>
#include <QTemporaryDir>
#include <sys/resource.h>
int main(int argc,char **argv) {
    if(argc>1 && QByteArray(argv[1])=="--session-worker") {
        QCoreApplication app(argc,argv);return WayWin::runWorker(app.arguments().mid(2));
    }
    QApplication app(argc,argv);QCoreApplication::setOrganizationName("WayWin");QCoreApplication::setApplicationName("WayWin");QCoreApplication::setApplicationVersion("0.3.0");
    Q_INIT_RESOURCE(validation);
    Q_INIT_RESOURCE(icons);
    struct rlimit core{0,0};::setrlimit(RLIMIT_CORE,&core);
    if(app.arguments().size()==3&&app.arguments()[1]=="--surface-worker")return WayWin::runSurfaces(app.arguments()[2]);
    if(app.arguments().contains("--smoke-test")) {
        QTemporaryDir temp;WayWin::Window window(temp.path());window.show();
        QTimer::singleShot(150,&app,[&]{if(app.arguments().contains("--screenshot"))window.grab().save(QDir::currentPath()+"/waywin-preview.png");app.quit();});return app.exec();
    }
    const auto root=WayWin::privateDirectory(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
    if(root.isEmpty())return 2;
    QLockFile lock(root+"/application.lock");lock.setStaleLockTime(0);
    if(!lock.tryLock()){QMessageBox::information(nullptr,"WayWin","이미 실행 중인 WayWin 창을 사용하세요.");return 1;}
    WayWin::Window window;window.show();return app.exec();
}
