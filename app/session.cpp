// SPDX-License-Identifier: Apache-2.0
#include "session.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QTimer>
#include <QNetworkInformation>
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
    monotonic.start();
    commandWriter.fault=[this]{resourceFailure();};
    commandWriter.flow=[this](bool blocked){updateFlow(0,blocked);};
    commandWriter.load=[this](bool low){if(low)busyStages.remove(0);else busyStages.insert(0);updateFlow(0,blockedStages.contains(0));};
    commandWriter.superseded=[this](uint32_t id){requests.remove(id);inputRequests.remove(id);inputDeadlines.remove(id);};
    connect(&surfaceWorker,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus status){if(!stopping&&(code!=0||status==QProcess::CrashExit))resourceFailure();});
    connect(&worker,&QProcess::readyReadStandardOutput,this,&Session::receive);
    connect(&worker,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,&Session::finish);
    connect(&worker,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart) {failed=true;detail="세션 관리 프로세스를 시작하지 못했습니다. 설치를 확인하세요.";finish(-1,QProcess::CrashExit);}
    });
    worker.setStandardErrorFile(QProcess::nullDevice());
    surfaceWorker.setStandardErrorFile(QProcess::nullDevice());
    connect(&surfaceWorker,&QProcess::readyReadStandardOutput,this,[this]{
        const auto data=surfaceWorker.readAllStandardOutput();
        try{for(const auto&m:surfaceCommands.feed(reinterpret_cast<const uint8_t*>(data.constData()),size_t(data.size())))
            if(m.epoch==epoch&&(m.type==WayWinWire::Pointer||m.type==WayWinWire::Key||m.type==WayWinWire::Text||m.type==WayWinWire::SurfaceClose||m.type==WayWinWire::SurfaceState))request(m.type,m.data);
            else if(m.epoch==epoch&&m.type==WayWinWire::Flow){WayWinWire::Reader read(m.data);auto value=read.number();if(value==3)busyStages.insert(3);else if(value==0)busyStages.remove(3);updateFlow(3,value==3?blockedStages.contains(3):value!=0);}
        }catch(...){surfaceWorker.kill();companionStatus="셸 화면 프로세스의 응답 오류. 화면을 다시 여세요.";emit companionChanged();}
    });
    auto timer=new QTimer(this);timer->setInterval(100);
    connect(timer,&QTimer::timeout,this,[this]{
        const auto now=QDateTime::currentMSecsSinceEpoch();bool expired=false;
        for(auto it=requests.begin();it!=requests.end();)if(it.value()<now){it=requests.erase(it);expired=true;}else ++it;
        for(auto it=inputDeadlines.cbegin();it!=inputDeadlines.cend();++it)if(it.value()<=monotonic.elapsed()){resourceFailure();break;}
        if(inputStall.expired(uint64_t(monotonic.elapsed())))resourceFailure();
        if(inputBlocked&&blockedStages.isEmpty()&&busyStages.isEmpty()&&requests.size()<int(WayWinPolicy::PendingRequests)&&commandWriter.low())updateFlow(0,false);
        if(expired){companionStatus="보조 앱 응답 시간 초과. Windows에서 WayWin Companion을 다시 열고 재시도하세요.";emit companionChanged();}
        if(active()&&channelReady&&state=="연결됨"&&now-lastHeartbeat>=5000){lastHeartbeat=now;request(WayWinWire::Hello);}
    });timer->start();
    auto lastTick=std::make_shared<qint64>(QDateTime::currentMSecsSinceEpoch());
    connect(timer,&QTimer::timeout,this,[this,lastTick]{auto now=QDateTime::currentMSecsSinceEpoch();if(now-*lastTick>15000&&(state=="재연결 중"||state=="재연결 대기"))recovery(23);*lastTick=now;});
    QNetworkInformation::loadDefaultBackend();
    if(auto network=QNetworkInformation::instance())connect(network,&QNetworkInformation::reachabilityChanged,this,[this](QNetworkInformation::Reachability reach){if(reach==QNetworkInformation::Reachability::Online&&(state=="재연결 중"||state=="재연결 대기"))recovery(23);});
}
Session::~Session() {
    if(surfaceWorker.state()!=QProcess::NotRunning){surfaceWorker.closeWriteChannel();if(!surfaceWorker.waitForFinished(1000)){surfaceWorker.kill();surfaceWorker.waitForFinished(1000);}}
    worker.closeWriteChannel();
    if(worker.state()!=QProcess::NotRunning) worker.waitForFinished(4500);
}
bool Session::active() const {return worker.state()!=QProcess::NotRunning;}
void Session::start(const QString &binary,QByteArray password,bool launch) {
    if(active())return;
    expectedDrivers=displayDrivers(QProcessEnvironment::systemEnvironment(),host.displayBackend);
    if(expectedDrivers.isEmpty()){password.fill('\0');state="실패";detail="선택한 화면 연결 방식을 사용할 수 없습니다. 호스트 설정과 그래픽 세션을 확인하세요.";emit changed();return;}
    runId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    runDirectory=privateDirectory(root+"/runs/"+runId);
    const auto certDir=privateDirectory(root+"/certificates/"+host.id);
    if(runDirectory.isEmpty() || certDir.isEmpty()) {state="실패";detail="실행 기록 또는 인증서 폴더를 만들 수 없습니다.";emit changed();return;}
    events.close();count=0;truncated=false;stopping=false;failed=false;pid=0;driver.clear();pending.clear();windows.clear();icons.clear();
    clearTransient(false);commandWriter.reset();blockedStages.clear();inputBlocked=false;inputStall.set(false,0);companionReady=false;channelReady=false;epoch=1;
    QJsonObject run{{"schema","waywin-run-1"},{"run_id",runId},{"host_id",host.id},
        {"started",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"mode",host.mode},{"program_start_requested",launch && host.mode=="rail"},{"environment",environment(host.displayBackend)}};
    QString error;
    if(!saveJson(runDirectory+"/run.json",run,&error)) {state="실패";detail=error;emit changed();return;}
    events.setFileName(runDirectory+"/events.jsonl");
    if(!events.open(QIODevice::WriteOnly|QIODevice::NewOnly) || !events.setPermissions(QFile::ReadOwner|QFile::WriteOwner)) {state="실패";detail="이벤트 기록을 열 수 없습니다.";emit changed();return;}
    auto processEnvironment=sessionEnvironment(certDir,host.displayBackend);processEnvironment.insert("WAYWIN_ACCENT",host.accent());worker.setProcessEnvironment(processEnvironment);
    state="연결 중"; detail="인증 및 서버 연결을 기다리고 있습니다.";
    worker.setProgram(QCoreApplication::applicationFilePath());
    worker.setArguments(QStringList{"--session-worker",binary}+arguments(host,launch));
    worker.start();
    password.append('\n'); commandWriter.enqueue({password.toStdString(),true}); password.fill('\0');
    emit changed();
}
void Session::stop() {
    if(!active())return;
    commandWriter.discardPending();
    stopping=true;state="연결 해제 중";if(!failed)detail="세션 프로세스를 정리하고 있습니다.";
    worker.closeWriteChannel();emit changed();
    const auto stoppingRun=runId;
    QTimer::singleShot(int(WayWinPolicy::GraceMs),this,[this,stoppingRun]{
        if(active()&&stopping&&runId==stoppingRun){if(pid>0){::kill(-pid,SIGCONT);::kill(-pid,SIGKILL);}worker.kill();}
    });
}
void Session::inject(const QString &action) {
    if(!active() || pid<=0)return;
    if(action=="pause") {if(state!="시험: 일시 정지")beforePause=state;::kill(-pid,SIGSTOP);state="시험: 일시 정지";}
    else if(action=="resume") {::kill(-pid,SIGCONT);if(!beforePause.isEmpty())state=beforePause;beforePause.clear();detail="세션 프로세스를 재개했습니다. 화면과 입력 반응을 확인하세요.";}
    else if(action=="crash") {::kill(-pid,SIGCONT);::kill(-pid,SIGKILL);}
    record({{"type","fault-injection"},{"action",action}});emit changed();
}
void Session::command(unsigned action, quint32 window, quint32 icon) {
    if(!active() || state!="연결됨" || action<1 || action>6)return;
    if(inputBlocked&&action!=4)return;
    if(action<=4 && !windows.contains(window))return;
    if(action>=5 && !icons.contains((quint64(window)<<32)|icon))return;
    const bool control=action==4;
    if(!control&&requests.size()>=int(WayWinPolicy::PendingRequests)){updateFlow(0,true);return;}
    if(control&&controlRequests.size()>=int(WayWinPolicy::ControlCommands)){resourceFailure();return;}
    const auto id=++nextRequest;
    auto line=QByteArray::number(action)+' '+QByteArray::number(window)+' '+QByteArray::number(icon)+' '+QByteArray::number(epoch)+' '+QByteArray::number(id)+'\n';
    if(!commandWriter.enqueue({line.toStdString(),control,false,window,epoch,id}))return;
    inputRequests.insert(id);inputDeadlines[id]=monotonic.elapsed()+qint64(WayWinPolicy::StallMs);
    if(control)controlRequests.insert(id);else requests[id]=QDateTime::currentMSecsSinceEpoch()+10000;
    if(requests.size()>=int(WayWinPolicy::PendingRequests))updateFlow(0,true);
}
void Session::recovery(unsigned action) {
    if(active() && action>=21 && action<=23)commandWriter.enqueue({(QByteArray::number(action)+" 0 0 "+QByteArray::number(epoch)+" -\n").toStdString(),true,false,0,epoch});
}
void Session::removeIcon(quint64 id) {
    icons.remove(id);trayTitles.remove(id);imageUse.remove(id);imageBytes-=trayImages.take(id).size();
}
void Session::clearTransient(bool preserveWindows) {
    if(!preserveWindows)windows.clear();
    icons.clear();windowTitles.clear();trayTitles.clear();trayImages.clear();imageUse.clear();imageBytes=0;
    requests.clear();inputRequests.clear();controlRequests.clear();inputDeadlines.clear();
    shellReady=false;commandWriter.discardPending();blockedStages.clear();busyStages.clear();inputBlocked=false;inputStall.set(false,0);
}
void Session::resourceFailure() {
    if(stopping)return;
    failed=true;detail="자원 제한 또는 입력 전송 시간 초과로 이 호스트 연결을 종료합니다. 직접 다시 연결하세요.";
    companionStatus=detail;stop();emit companionChanged();
}
void Session::updateFlow(unsigned stage,bool blocked) {
    if(blocked)blockedStages.insert(stage);else blockedStages.remove(stage);
    const bool value=!blockedStages.isEmpty()||requests.size()>=int(WayWinPolicy::PendingRequests)||(inputBlocked&&(!busyStages.isEmpty()||!commandWriter.low()));
    inputStall.set(value,uint64_t(monotonic.elapsed()));
    if(inputBlocked==value)return;
    inputBlocked=value;
    if(surfaceWorker.state()!=QProcess::NotRunning){WayWinWire::Message m{WayWinWire::Flow,0,epoch,{}};WayWinWire::put(m.data,value?1:0);auto bytes=WayWinWire::encode(m);surfaceWorker.write(reinterpret_cast<const char*>(bytes.data()),qint64(bytes.size()));}
}
quint32 Session::request(quint32 type,const std::vector<uint8_t>&data) {
    using namespace WayWinWire;
    if(!requestType(type)||data.size()>65552)return 0;
    Message m{type,++nextRequest,epoch,data};
    auto packet=WayWinPolicy::packet({},m);
    if(inputBlocked&&!packet.control&&type!=SurfaceState)return 0;
    if(surfaceProxy){
        if(!proxyWriter){
            proxyWriter=new CommandWriter(STDOUT_FILENO,this);proxyWriter->fault=[]{QCoreApplication::exit(3);};
            auto report=[this](unsigned value){Message flow{Flow,0,epoch,{}};put(flow.data,value);auto bytes=encode(flow);proxyWriter->enqueue({std::string(bytes.begin(),bytes.end()),true,false,0,epoch});};
            proxyWriter->flow=[this,report](bool blocked){proxyBlocked=blocked;inputBlocked=upstreamBlocked||proxyBlocked;report(blocked?1:0);};
            proxyWriter->load=[report](bool low){report(low?0:3);};
        }
        auto bytes=encode(m);packet.bytes.assign(bytes.begin(),bytes.end());return proxyWriter->enqueue(std::move(packet))?m.request:0;
    }
    if(!active()||(!companionReady&&type!=Hello)||state!="연결됨")return 0;
    if((WayWinPolicy::input(type)||type==Shell||type==SurfaceClose||type==SurfaceState)&&!shellReady)return 0;
    if(!packet.control&&requests.size()>=int(WayWinPolicy::PendingRequests)){updateFlow(0,true);return 0;}
    if(packet.control&&controlRequests.size()>=int(WayWinPolicy::ControlCommands)){resourceFailure();return 0;}
    if(type==Hello){m.data.clear();put(m.data,RequestedCapabilities);}
    auto bytes=encode(m);
    packet.bytes="20 0 0 "+std::to_string(epoch)+" "+hex(bytes)+"\n";
    if(!commandWriter.enqueue(std::move(packet)))return 0;
    const auto deadline=QDateTime::currentMSecsSinceEpoch();
    if(WayWinPolicy::input(type)||type==SurfaceClose||type==SurfaceState){
        inputRequests.insert(m.request);inputDeadlines[m.request]=monotonic.elapsed()+qint64(WayWinPolicy::StallMs);
    }
    if(WayWinPolicy::packet({},m).control)controlRequests.insert(m.request);
    else requests[m.request]=deadline+10000;
    if(requests.size()>=int(WayWinPolicy::PendingRequests))updateFlow(0,true);
    return m.request;
}
void Session::forwardSurface(const QByteArray&bytes) {
    if(surfaceWorker.state()==QProcess::NotRunning) {
        surfaceCommands.reset();surfaceWorker.setProgram(QCoreApplication::applicationFilePath());surfaceWorker.setArguments({"--surface-worker",host.name});
        auto env=QProcessEnvironment::systemEnvironment();
        env.insert("WAYWIN_ACCENT",host.accent());
#ifdef Q_OS_LINUX
        env.insert("QT_QPA_PLATFORM",driver=="x11"?"xcb":"wayland");
        const auto parent=::getpid();surfaceWorker.setChildProcessModifier([parent]{if(::prctl(PR_SET_PDEATHSIG,SIGKILL)!=0||::getppid()!=parent)::_exit(125);});
#endif
        surfaceWorker.setProcessEnvironment(env);surfaceWorker.start();
    }
    if(surfaceWorker.bytesToWrite()>4*WayWinWire::Maximum)return;
    surfaceWorker.write(bytes);
}
void Session::receiveTransient(const QByteArray &bytes) {
    using namespace WayWinWire;
    try {
        Decoder decoder;auto messages=decoder.feed(reinterpret_cast<const uint8_t*>(bytes.constData()),size_t(bytes.size()));
        if(messages.size()!=1)return;
        const auto &m=messages[0];Reader read(m.data);
        if(m.epoch<epoch)return;
        if(m.epoch>epoch){epoch=m.epoch;clearTransient(true);companionReady=false;}
        if(m.type==Flow){auto stage=read.number(),value=read.number();if(value==2){resourceFailure();return;}if(value==3)busyStages.insert(stage);else if(value==0)busyStages.remove(stage);updateFlow(stage,value==3?blockedStages.contains(stage):value!=0);return;}
        if(m.type==InputAck){if(!inputRequests.remove(m.request))return;inputDeadlines.remove(m.request);requests.remove(m.request);controlRequests.remove(m.request);if(requests.size()<int(WayWinPolicy::PendingRequests))updateFlow(0,false);return;}
        if(m.type==Channel){companionReady=false;clearTransient(true);channelReady=read.number()!=0;companionStatus=channelReady?"보조 앱 확인 중…":"보조 앱 채널이 닫혔습니다. Windows에서 Companion을 실행하세요.";}
        else if(m.type==Ready){auto sid=read.number();auto instance=QString::fromStdString(read.text(128));auto capabilities=read.number();
            requests.remove(m.request);channelReady=true;
            const auto identity=QString::number(sid)+":"+instance;
            if(!companionIdentity.isEmpty()&&companionIdentity!=identity){detail="Windows 세션 또는 보조 앱 인스턴스가 변경되었습니다. 원격 작업이 보존됐는지 확인하세요.";}
            companionIdentity=identity;companionReady=(capabilities&0x3f)==0x3f;shellReady=companionReady&&(capabilities&RequestedCapabilities)==RequestedCapabilities;companionStatus=companionReady&&!shellReady?"셸 조작을 사용하려면 Windows Companion을 함께 제공된 버전으로 업데이트하세요.":companionReady?"보조 앱 연결됨 · Windows 세션 "+QString::number(sid):"보조 앱 버전이 호환되지 않습니다. 함께 제공된 버전으로 설치하세요.";
        } else if(m.type==Retry){auto status=read.number(),attempt=read.number(),seconds=read.number(),code=read.number();
            if(status==4){emit closeRequested();return;}
            if(status!=0)shellReady=false;
            state=status==0?"연결됨":status==1?"재연결 중":status==2||status==5?"재연결 대기":"연결 실패";
            detail=status==0?"기존 세션에 다시 연결했습니다.":QString("시도 %1/5 · %2초 후 재시도 · 오류 %3. 재시도/중단 버튼을 사용할 수 있습니다.").arg(attempt).arg(seconds).arg(code);
            if(status!=0){companionReady=false;channelReady=false;}
            record({{"type","recovery"},{"status",int(status)},{"attempt",int(attempt)},{"code",double(code)}});
        } else if(m.type==Fault){auto code=read.number();failed=true;detail="Windows 앱 실행이 거부되었습니다. RemoteApp 별칭·허용 목록·Windows 에디션/정책을 확인하세요. 코드: "+QString::number(code);}
        else if(m.type==Title||m.type==TrayTitle){auto icon=read.number();auto title=QString::fromUtf8(read.text(4096));if(m.type==TrayTitle){auto id=(quint64(m.request)<<32)|icon;if(icons.contains(id))trayTitles[id]=title;}else if(windows.contains(m.request))windowTitles[m.request]=title;}
        else if(m.type==Icon){auto icon=read.number(),w=read.number(),h=read.number();auto id=(quint64(m.request)<<32)|icon;
            if(icons.contains(id)&&w&&h&&w<=256&&h<=256&&m.data.size()==12+quint64(w)*h*4){
                imageBytes-=trayImages.take(id).size();imageUse.remove(id);
                while(imageBytes+bytes.size()>qsizetype(WayWinPolicy::ImageBytes)&&!imageUse.isEmpty()){
                    auto oldest=std::min_element(imageUse.begin(),imageUse.end());auto evict=oldest.key();imageUse.erase(oldest);imageBytes-=trayImages.take(evict).size();
                }
                trayImages[id]=bytes;imageUse[id]=++imageClock;imageBytes+=bytes.size();
            }}
        else {
            if(m.type==AppList||m.type==Clipboard||m.type==Result){const bool known=requests.remove(m.request)||controlRequests.remove(m.request)||inputRequests.contains(m.request);inputRequests.remove(m.request);inputDeadlines.remove(m.request);controlRequests.remove(m.request);if(!known)return;
                if(requests.size()<int(WayWinPolicy::PendingRequests))updateFlow(0,false);}
            if(m.type==Result){auto code=read.number();companionStatus=(code?"작업 실패: ":"완료: ")+QString::fromUtf8(read.text(4096));}
            if(m.type==SurfaceGone&&!read.done())companionStatus="원본 셸 화면을 닫았습니다: "+QString::fromStdString(read.text(4096));
            if(m.type==Frame){if(shellReady)forwardSurface(bytes);}
            else if(m.type==SurfaceGone){if(surfaceWorker.state()!=QProcess::NotRunning)surfaceWorker.write(bytes);}
            else emit transient(bytes);
        }
        if((m.type==Channel||m.type==Retry)&&surfaceWorker.state()!=QProcess::NotRunning)surfaceWorker.write(bytes);
        emit companionChanged();if(m.type!=Frame)emit changed();
    }catch(...){companionStatus="보조 앱 응답 형식 오류. 함께 제공된 버전으로 다시 설치하세요.";emit companionChanged();}
}
void Session::record(QJsonObject event) {
    if(count>=100000) {truncated=true;return;}
    event["run_id"]=runId;event["sequence"]=++count;
    event["time"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    auto bytes=QJsonDocument(event).toJson(QJsonDocument::Compact)+'\n';
    if(events.write(bytes)!=bytes.size() || !events.flush()) {failed=true;detail="실행 기록 저장에 실패했습니다.";stop();}
}
void Session::receive() {
    if(worker.isOpen())pending+=worker.readAllStandardOutput();
    while(pending.contains('\n')) {
        const auto end=pending.indexOf('\n');auto line=pending.left(end);pending.remove(0,end+1);
        if(line.size()>2*(WayWinWire::Maximum+24)+128)continue;
        const auto event=QJsonDocument::fromJson(line).object();
        const auto type=event["type"].toString();
        if(type=="ui") {receiveTransient(QByteArray::fromHex(event["payload"].toString().toLatin1()));continue;}
        if(type=="process") {pid=static_cast<qint64>(event["pid"].toDouble());continue;}
        if(type=="driver") {
            driver=event["driver"].toString();
            if(!expectedDrivers.contains(driver)) {failed=true;detail="선택한 방식과 다른 화면 드라이버입니다. 호스트 설정과 세션 엔진을 확인하세요.";stop();}
        } else if(type=="connected" && !stopping) {
            if(!expectedDrivers.contains(driver)){failed=true;detail="실제 화면 드라이버를 확인하지 못했습니다. 세션 엔진 설치를 확인하세요.";stop();}
            else {state="연결됨";detail="화면 연결: "+driver+" · 원격 화면에서 입력과 창 동작을 확인하세요.";}
        }
        else if(type=="disconnected" && !stopping && !failed) {
            state="연결 끊김";
            if(event["code"].toDouble()==0x00020001)
                detail="서버 접속 전 초기화에 실패했습니다. 최신 설치 묶음으로 다시 설치하고 화면 연결·모니터 설정을 확인하세요. 오류 코드: 131073 (ERRCONNECT_PRE_CONNECT_FAILED)";
            else
                detail="인증·인증서·서버 상태를 확인한 뒤 다시 연결하세요. 오류 코드: "+QString::number(event["code"].toDouble(),'f',0);
        }
        else if(type=="window-create"){
            const auto id=quint32(event["window"].toDouble());
            if(!windows.contains(id)&&windows.size()>=int(WayWinPolicy::IdLimit))resourceFailure();else windows.insert(id);
        }else if(type=="window-delete"){
            const auto id=quint32(event["window"].toDouble());windows.remove(id);windowTitles.remove(id);
            for(auto icon:QSet<quint64>(icons))if(quint32(icon>>32)==id)removeIcon(icon);
        }else if(type=="notify-create"){
            auto id=(quint64(event["window"].toDouble())<<32)|quint32(event["icon"].toDouble());
            if(!icons.contains(id)&&icons.size()>=int(WayWinPolicy::IdLimit))resourceFailure();else icons.insert(id);
        }else if(type=="notify-delete")removeIcon((quint64(event["window"].toDouble())<<32)|quint32(event["icon"].toDouble()));
        if(!type.isEmpty())record(event);
        emit changed();
    }
    if(pending.size()>2*(WayWinWire::Maximum+24)+256)pending.clear();
}
QJsonObject Session::summary() const {
    return {{"run_id",runId},{"host_id",host.id},{"sdl_driver_observed",driver},{"events",count},{"events_truncated",truncated}};
}
void Session::finish(int code,QProcess::ExitStatus status) {
    receive();
    state=stopping && !failed ? "연결 해제됨" : (status==QProcess::NormalExit && code==0 && !failed ? "세션 종료됨" : "연결 실패");
    if(!stopping && (status!=QProcess::NormalExit || code!=0) && !failed && !detail.contains("오류 코드"))
        detail=driver.isEmpty()?"화면 드라이버를 초기화하지 못했거나 엔진이 시작되지 않았습니다. 화면 연결 설정·서버 접근 권한·엔진 설치를 확인하세요.":"연결에 실패했습니다. 주소·계정·인증서와 서버 상태를 확인하고 다시 연결하세요.";
    auto o=summary();o["exit_code"]=code;o["ended"]=stopping?"interrupted":"session-exit";
    QString error;if(!runDirectory.isEmpty() && !saveJson(runDirectory+"/exit.json",o,&error))detail=error;
    events.close();pid=0;
    companionReady=false;channelReady=false;clearTransient(false);commandWriter.reset();
    surfaceWorker.closeWriteChannel();
    emit companionChanged();emit changed();emit ended();
    if(!stopping&&!failed&&(status==QProcess::CrashExit||code==128)) {
        if(crashRestarts++==0){detail="세션 엔진이 종료되어 한 번 복구합니다. 원격 앱은 새로 실행하지 않습니다.";emit changed();QTimer::singleShot(0,this,&Session::crashRecoveryRequested);}
        else{detail="세션 엔진이 반복 종료됐습니다. 환경·엔진 설치를 확인한 뒤 직접 다시 연결하세요.";emit changed();}
    }
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
    CommandWriter childCommands(&child);
    QByteArray commandLine;
    qint64 group=0;
    bool started=false,ending=false;
    QByteArray password;
    EventDecoder decoder;
    auto output=[](const QJsonObject &o){const auto s=QJsonDocument(o).toJson(QJsonDocument::Compact)+'\n';qsizetype offset=0;while(offset<s.size()){auto n=::write(STDOUT_FILENO,s.constData()+offset,size_t(s.size()-offset));if(n<0&&errno==EINTR)continue;if(n<=0){workerStop=1;break;}offset+=n;}};
    QTimer killTimer;killTimer.setSingleShot(true);
    auto stop=[&](){
        if(ending)return;
        ending=true;
        if(group>0) {::kill(-group,SIGCONT);::kill(-group,SIGTERM);killTimer.start(3000);}
        else if(!started)QCoreApplication::exit(130);
        else child.terminate();
    };
    QObject::connect(&killTimer,&QTimer::timeout,&child,[&](){if(group>0)::kill(-group,SIGKILL);child.kill();});
    auto localMessage=[&](WayWinWire::Message m){auto bytes=WayWinWire::encode(m);output({{"type","ui"},{"payload",QString::fromStdString(WayWinWire::hex(bytes))}});};
    quint32 commandEpoch=0;
    childCommands.flow=[&](bool blocked){WayWinWire::Message m{WayWinWire::Flow,0,commandEpoch,{}};WayWinWire::put(m.data,1);WayWinWire::put(m.data,blocked?1:0);localMessage(std::move(m));};
    childCommands.load=[&](bool low){WayWinWire::Message m{WayWinWire::Flow,0,commandEpoch,{}};WayWinWire::put(m.data,1);WayWinWire::put(m.data,low?0:3);localMessage(std::move(m));};
    childCommands.fault=[&]{WayWinWire::Message m{WayWinWire::Flow,0,commandEpoch,{}};WayWinWire::put(m.data,1);WayWinWire::put(m.data,2);localMessage(std::move(m));stop();};
    childCommands.superseded=[&](uint32_t id){localMessage({WayWinWire::InputAck,id,commandEpoch,{}});};
    QSocketNotifier input(STDIN_FILENO,QSocketNotifier::Read);
    QObject::connect(&input,&QSocketNotifier::activated,&child,[&](){
        char bytes[1024];const auto count=::read(STDIN_FILENO,bytes,sizeof(bytes));
        if(count<=0){input.setEnabled(false);stop();return;}
        if(started){
            if(ending)return;
            for(int i=0;i<count;++i){
                if(bytes[i]=='\n'){
                    try{auto packet=WayWinPolicy::commandPacket(commandLine.toStdString());
                        if(packet.epoch!=commandEpoch){commandEpoch=packet.epoch;childCommands.discardPending();}
                        childCommands.enqueue(std::move(packet));
                    }catch(...){stop();}
                    commandLine.clear();
                }else if(commandLine.size()<qint64(WayWinPolicy::CommandLineBytes))commandLine+=bytes[i];else {stop();return;}
            }
            return;
        }
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
        child.start();childCommands.enqueue({password.toStdString(),true});password.fill('\0');password.clear();
    });
    QObject::connect(&child,&QProcess::started,&child,[&](){group=child.processId();output({{"type","process"},{"pid",double(group)}});if(ending){::kill(-group,SIGTERM);killTimer.start(3000);}});
    QObject::connect(&child,&QProcess::readyReadStandardError,&child,[&](){for(const auto &e:decoder.feed(child.readAllStandardError())){
        output(e);
        if(e["type"]=="ui")try{const auto bytes=QByteArray::fromHex(e["payload"].toString().toLatin1());WayWinWire::Decoder wire;
            for(const auto&m:wire.feed(reinterpret_cast<const uint8_t*>(bytes.constData()),size_t(bytes.size())))if(m.type==WayWinWire::Flow){WayWinWire::Reader r(m.data);r.number();if(r.number()==2)stop();}
        }catch(...){stop();}
    }});
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
