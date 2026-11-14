// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core.h"
#include <QObject>
#include <QProcess>
#include <QFile>
#include <QSet>
namespace WayWin {
class Session : public QObject {
    Q_OBJECT
public:
    explicit Session(const Host &host, const QString &dataRoot, QObject *parent=nullptr);
    ~Session() override;
    Host host;
    QString state="연결 안 됨", detail, runId, runDirectory;
    QString driver;
    QSet<quint32> windows;
    QSet<quint64> icons;
    bool active() const;
    void start(const QString &binary, QByteArray password, bool launch);
    void stop();
    void inject(const QString &action);
    void command(unsigned action, quint32 window, quint32 icon = 0);
    QJsonObject summary() const;
signals:
    void changed();
    void ended();
private:
    QString root;
    QProcess worker;
    QByteArray pending;
    QFile events;
    qint64 pid=0;
    bool stopping=false, failed=false;
    int count=0;
    bool truncated=false;
    void receive();
    void record(QJsonObject event);
    void finish(int code, QProcess::ExitStatus status);
};
int runWorker(const QStringList &arguments);
}
