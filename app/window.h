// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "session.h"
#include <QMainWindow>
#include <QMap>
class QListWidget;
class QLabel;
class QTabWidget;
class QPushButton;
class QComboBox;
class QCheckBox;
class QCloseEvent;
namespace WayWin {
class Validation;
class Window : public QMainWindow {
    Q_OBJECT
public:
    explicit Window(const QString &dataRoot={},QWidget *parent=nullptr);
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    QString root,runtime;
    QVector<Host> hosts;
    QMap<QString,Session*> sessions;
    QSet<QString> busy;
    QListWidget *list,*windows;
    QLabel *title,*state,*detail,*environmentLabel,*runtimeLabel;
    QComboBox *mode;
    QCheckBox *launch;
    QPushButton *connectButton,*disconnectButton,*editButton,*deleteButton;
    QTabWidget *tabs;
    Validation *validation;
    bool closing=false,configWritable=true;
    Host *selected();
    bool save();
    void refresh();
    void editHost(bool create);
    void removeHost();
    void connectHost(bool reconnect=false);
    void authenticate(const Host &host,const QString &binary,bool launchApp);
    void begin(const Host &host,const QString &binary,QByteArray password,bool launchApp);
    void showError(const QString &error);
    void checkEnvironment();
    void listMonitors();
    void switchMode();
};
}
