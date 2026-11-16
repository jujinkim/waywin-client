// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core.h"
#include <QObject>
#include <QProcess>
#include <QFile>
#include <QSet>
#include <QMap>
#include "../protocol/waywin_wire.hpp"
#include "transport.h"
class ProductTests;
namespace WayWin {
class Session : public QObject {
    Q_OBJECT
    friend class ::ProductTests;
public:
    explicit Session(const Host &host, const QString &dataRoot, QObject *parent=nullptr);
    ~Session() override;
    Host host;
    QString state="연결 안 됨", detail, runId, runDirectory;
    QString driver;
    QSet<quint32> windows;
    QSet<quint64> icons;
    QMap<quint32,QString> windowTitles;
    QMap<quint64,QByteArray> trayImages;
    QMap<quint64,QString> trayTitles;
    QString companionIdentity,companionStatus="Windows 보조 앱을 기다리고 있습니다.";
    quint32 epoch=1;
    bool companionReady=false;
    bool shellReady=false, inputBlocked=false;
    bool surfaceProxy=false;
    bool active() const;
    void start(const QString &binary, QByteArray password, bool launch);
    void stop();
    void inject(const QString &action);
    void command(unsigned action, quint32 window, quint32 icon = 0);
    quint32 request(quint32 type,const std::vector<uint8_t>& data={});
    void recovery(unsigned action);
    void discardInput(){if(proxyWriter)proxyWriter->discardPending();}
    void proxyFlow(bool value){upstreamBlocked=value;inputBlocked=upstreamBlocked||proxyBlocked;}
    QByteArray trayImage(quint64 id){if(trayImages.contains(id))imageUse[id]=++imageClock;return trayImages.value(id);}
    QJsonObject summary() const;
signals:
    void changed();
    void ended();
    void transient(const QByteArray &bytes);
    void companionChanged();
    void crashRecoveryRequested();
    void closeRequested();
private:
    QString root;
    QStringList expectedDrivers;
    QString beforePause;
    QProcess worker;
    QProcess surfaceWorker;
    CommandWriter commandWriter{&worker};
    CommandWriter *proxyWriter=nullptr;
    WayWinWire::Decoder surfaceCommands;
    QByteArray pending;
    QFile events;
    qint64 pid=0;
    bool stopping=false, failed=false;
    int count=0;
    bool truncated=false;
    quint32 nextRequest=10;
    QMap<quint32,qint64> requests;
    QSet<quint32> inputRequests;
    QSet<quint32> controlRequests;
    QMap<quint32,qint64> inputDeadlines;
    QMap<quint64,quint64> imageUse;
    quint64 imageClock=0;
    qsizetype imageBytes=0;
    QSet<unsigned> blockedStages;
    QSet<unsigned> busyStages;
    bool upstreamBlocked=false,proxyBlocked=false;
    WayWinPolicy::Stall inputStall;
    QElapsedTimer monotonic;
    unsigned crashRestarts=0;
    bool channelReady=false;
    qint64 lastHeartbeat=0;
    void clearTransient(bool preserveWindows);
    void removeIcon(quint64 id);
    void resourceFailure();
    void updateFlow(unsigned stage,bool blocked);
    void receiveTransient(const QByteArray &bytes);
    void forwardSurface(const QByteArray &bytes);
    void receive();
    void record(QJsonObject event);
    void finish(int code, QProcess::ExitStatus status);
};
int runWorker(const QStringList &arguments);
}
