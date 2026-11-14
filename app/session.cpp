// SPDX-License-Identifier: Apache-2.0
#include "session.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QTimer>
#include <QUuid>
#include <csignal>
#include <cstdio>
#include <unistd.h>
#include <sys/resource.h>
#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#endif
namespace WayWin {
Session::Session(const Host &h,const QString &dataRoot,QObject *parent):QObject(parent),host(h),root(dataRoot) {
    connect(&worker,&QProcess::readyReadStandardOutput,this,&Session::receive);
    connect(&worker,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,&Session::finish);
    connect(&worker,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart) {failed=true;detail="세션 관리 프로세스를 시작하지 못했습니다. 설치를 확인하세요.";finish(-1,QProcess::CrashExit);}
    });
    worker.setStandardErrorFile(QProcess::nullDevice());
}
Session::~Session() {
    worker.closeWriteChannel();
    if(worker.state()!=QProcess::NotRunning) worker.waitForFinished(4500);
}
bool Session::active() const {return worker.state()!=QProcess::NotRunning;}
void Session::start(const QString &binary,QByteArray password,bool launch) {
    if(active())return;
    runId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    runDirectory=privateDirectory(root+"/runs/"+runId);
    const auto certDir=privateDirectory(root+"/certificates/"+host.id);
    if(runDirectory.isEmpty() || certDir.isEmpty()) {state="실패";detail="실행 기록 또는 인증서 폴더를 만들 수 없습니다.";emit changed();return;}
    events.close();count=0;truncated=false;stopping=false;failed=false;pid=0;driver.clear();pending.clear();windows.clear();icons.clear();
    QJsonObject run{{"schema","waywin-run-1"},{"run_id",runId},{"host_id",host.id},
        {"started",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"mode",host.mode},{"program_start_requested",launch && host.mode=="rail"},{"environment",environment()}};
    QString error;
    if(!saveJson(runDirectory+"/run.json",run,&error)) {state="실패";detail=error;emit changed();return;}
    events.setFileName(runDirectory+"/events.jsonl");
    if(!events.open(QIODevice::WriteOnly|QIODevice::NewOnly) || !events.setPermissions(QFile::ReadOwner|QFile::WriteOwner)) {state="실패";detail="이벤트 기록을 열 수 없습니다.";emit changed();return;}
    worker.setProcessEnvironment(sessionEnvironment(certDir));
    state="연결 중"; detail="인증 및 서버 연결을 기다리고 있습니다.";
    worker.setProgram(QCoreApplication::applicationFilePath());
    worker.setArguments(QStringList{"--session-worker",binary}+arguments(host,launch));
    worker.start();
    password.append('\n'); worker.write(password); password.fill('\0');
    emit changed();
}
void Session::stop() {
    if(!active())return;
    stopping=true;state="연결 해제 중";detail="세션 프로세스를 정리하고 있습니다.";
    worker.closeWriteChannel();emit changed();
}
void Session::inject(const QString &action) {
    if(!active() || pid<=0)return;
    if(action=="pause") {::kill(-pid,SIGSTOP);state="시험: 일시 정지";}
    else if(action=="resume") {::kill(-pid,SIGCONT);state="연결 상태 재확인 필요";}
    else if(action=="crash") {::kill(-pid,SIGCONT);::kill(-pid,SIGKILL);}
    record({{"type","fault-injection"},{"action",action}});emit changed();
}
void Session::command(unsigned action, quint32 window, quint32 icon) {
    if(!active() || state!="연결됨" || action<1 || action>6)return;
    if(action<=4 && !windows.contains(window))return;
    if(action>=5 && !icons.contains((quint64(window)<<32)|icon))return;
    worker.write(QByteArray::number(action)+' '+QByteArray::number(window)+' '+QByteArray::number(icon)+'\n');
}
void Session::record(QJsonObject event) {
    if(count>=100000) {truncated=true;return;}
    event["run_id"]=runId;event["sequence"]=++count;
    event["time"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    auto bytes=QJsonDocument(event).toJson(QJsonDocument::Compact)+'\n';
    if(events.write(bytes)!=bytes.size() || !events.flush()) {failed=true;detail="실행 기록 저장에 실패했습니다.";stop();}
}
void Session::receive() {
    pending+=worker.readAllStandardOutput();
    while(pending.contains('\n')) {
        const auto end=pending.indexOf('\n');auto line=pending.left(end);pending.remove(0,end+1);
        if(line.size()>1024)continue;
        const auto event=QJsonDocument::fromJson(line).object();
        const auto type=event["type"].toString();
        if(type=="process") {pid=static_cast<qint64>(event["pid"].toDouble());continue;}
        if(type=="driver") {
            driver=event["driver"].toString();
            if(driver!="wayland") {failed=true;detail="네이티브 Wayland 드라이버가 아닙니다. GNOME Wayland로 로그인하세요.";stop();}
        } else if(type=="connected" && !stopping) {state="연결됨";detail="원격 화면에서 입력과 창 동작을 확인하세요.";}
        else if(type=="disconnected" && !stopping) {state="연결 끊김";detail="인증·인증서·서버 상태를 확인한 뒤 다시 연결하세요. 오류 코드: "+QString::number(event["code"].toDouble(),'f',0);}
        else if(type=="window-create")windows.insert(event["window"].toDouble());
        else if(type=="window-delete")windows.remove(event["window"].toDouble());
        else if(type=="notify-create")icons.insert((quint64(event["window"].toDouble())<<32)|quint32(event["icon"].toDouble()));
        else if(type=="notify-delete")icons.remove((quint64(event["window"].toDouble())<<32)|quint32(event["icon"].toDouble()));
        if(!type.isEmpty())record(event);
        emit changed();
    }
    if(pending.size()>2048)pending.clear();
}
QJsonObject Session::summary() const {
    return {{"run_id",runId},{"host_id",host.id},{"sdl_driver_observed",driver},{"events",count},{"events_truncated",truncated}};
}
void Session::finish(int code,QProcess::ExitStatus status) {
    receive();
    state=stopping && !failed ? "연결 해제됨" : (status==QProcess::NormalExit && code==0 && !failed ? "세션 종료됨" : "연결 실패");
    if(!stopping && (status!=QProcess::NormalExit || code!=0) && !failed && !detail.contains("오류 코드"))detail="연결에 실패했습니다. 주소·계정·인증서와 서버 상태를 확인하고 다시 연결하세요.";
    auto o=summary();o["exit_code"]=code;o["ended"]=stopping?"interrupted":"session-exit";
    QString error;if(!runDirectory.isEmpty() && !saveJson(runDirectory+"/exit.json",o,&error))detail=error;
    events.close();pid=0;emit changed();emit ended();
}
static volatile std::sig_atomic_t workerStop=0;
static void stopSignal(int) {workerStop=1;}
int runWorker(const QStringList &args) {
    if(args.isEmpty())return 2;
    ::setsid();
    std::signal(SIGTERM,stopSignal);std::signal(SIGINT,stopSignal);std::signal(SIGHUP,stopSignal);
    std::signal(SIGPIPE,SIG_IGN);
    struct rlimit limit{0,0};::setrlimit(RLIMIT_CORE,&limit);
    QProcess child;
    qint64 group=0;
    bool started=false,ending=false;
    QByteArray password;
    EventDecoder decoder;
    auto output=[](const QJsonObject &o){const auto s=QJsonDocument(o).toJson(QJsonDocument::Compact)+'\n';if(::write(STDOUT_FILENO,s.constData(),s.size())<0)workerStop=1;};
    QTimer killTimer;killTimer.setSingleShot(true);
    auto stop=[&](){
        if(ending)return;
        ending=true;
        if(group>0) {::kill(-group,SIGCONT);::kill(-group,SIGTERM);killTimer.start(3000);}
        else if(!started)QCoreApplication::exit(130);
        else child.terminate();
    };
    QObject::connect(&killTimer,&QTimer::timeout,&child,[&](){if(group>0)::kill(-group,SIGKILL);child.kill();});
    QSocketNotifier input(STDIN_FILENO,QSocketNotifier::Read);
    QObject::connect(&input,&QSocketNotifier::activated,&child,[&](){
        char bytes[1024];const auto count=::read(STDIN_FILENO,bytes,sizeof(bytes));
        if(count<=0){input.setEnabled(false);stop();return;}
        if(started){ child.write(bytes,count); return; }
        password.append(bytes,count);std::fill(std::begin(bytes),std::end(bytes),0);
        if(password.size()>1024){password.fill('\0');QCoreApplication::exit(2);return;}
        if(!password.contains('\n'))return;
        if(password.indexOf('\n')!=password.size()-1){QCoreApplication::exit(2);return;}
        started=true;
        child.setProgram(args.first());child.setArguments(args.mid(1));
        child.setStandardOutputFile(QProcess::nullDevice());
        const auto expectedParent=::getpid();
        child.setChildProcessModifier([expectedParent](){
            (void)expectedParent;
            ::setsid();
#ifdef Q_OS_LINUX
            if(::prctl(PR_SET_PDEATHSIG,SIGKILL)!=0 || ::getppid()!=expectedParent)::_exit(125);
#endif
            struct rlimit core{0,0};::setrlimit(RLIMIT_CORE,&core);
        });
        child.start();child.write(password);password.fill('\0');password.clear();
    });
    QObject::connect(&child,&QProcess::started,&child,[&](){group=child.processId();output({{"type","process"},{"pid",double(group)}});if(ending){::kill(-group,SIGTERM);killTimer.start(3000);}});
    QObject::connect(&child,&QProcess::readyReadStandardError,&child,[&](){for(const auto &e:decoder.feed(child.readAllStandardError()))output(e);});
    QObject::connect(&child,&QProcess::errorOccurred,&child,[&](QProcess::ProcessError e){if(e==QProcess::FailedToStart)QCoreApplication::exit(127);});
    QObject::connect(&child,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),&child,[&](int code,QProcess::ExitStatus status){
        for(const auto &e:decoder.feed(child.readAllStandardError()))output(e);
        if(group>0)::kill(-group,SIGKILL);
        QCoreApplication::exit(ending?130:(status==QProcess::CrashExit?128:code));
    });
    QTimer poll;QObject::connect(&poll,&QTimer::timeout,&child,[&](){if(workerStop)stop();});poll.start(100);
    return QCoreApplication::exec();
}
}
