// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "session.h"
#include <QWidget>
#include <QPointer>
class QLabel;
class QListWidget;
class QPlainTextEdit;
namespace WayWin {
int runSurfaces(const QString &name);
class Companion : public QWidget {
    Q_OBJECT
public:
    explicit Companion(QWidget *parent=nullptr);
    void setSession(Session*);
private:
    QPointer<Session> session;
    QLabel *status;
    QListWidget *apps;
    QPlainTextEdit *clipboard;
    QWidget *actions;
    QWidget *shellActions;
    QMap<Session*,QMap<quint32,QPointer<QWidget>>> surfaces;
    QMap<QString,QString> clipboardDrafts;
    void message(Session*,const QByteArray&);
    void refresh();
};
}
