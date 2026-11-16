// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../protocol/waywin_policy.hpp"
#include <QObject>
#include <QProcess>
#include <QSocketNotifier>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
namespace WayWin {
// Own the application backlog and allow only one bounded command in Qt's buffer.
// fd output is nonblocking so an unread GUI pipe cannot stop the event loop.
class CommandWriter : public QObject {
    WayWinPolicy::Queue queue;
    QProcess *process=nullptr;
    int fd=-1;
    QSocketNotifier *writable=nullptr;
    size_t submitted=0,confirmed=0;
    bool blocked=false,dead=false,wasLow=true;
    QElapsedTimer progress;
    QTimer timer;
    void update() {
        const bool isLow=queue.low();
        if(wasLow!=isLow){wasLow=isLow;if(load)load(isLow);}
        const bool value=queue.high()||(blocked&&!queue.low());
        if(blocked!=value){blocked=value;if(flow)flow(blocked);}
    }
    void fail(){if(dead)return;dead=true;if(fault)fault();}
    void pump() {
        if(dead)return;
        while(!queue.empty()) {
            const auto &bytes=queue.front().bytes;
            if(process){
                if(process->state()!=QProcess::Running)return;
                if(submitted<bytes.size()){
                    auto n=process->write(bytes.data()+submitted,qint64(bytes.size()-submitted));
                    if(n<0){fail();return;}submitted+=size_t(n);
                }
                return;
            }
            auto n=::write(fd,bytes.data()+submitted,bytes.size()-submitted);
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){writable->setEnabled(true);return;}
            if(n<0&&errno==EINTR)continue;
            if(n<=0){fail();return;}
            submitted+=size_t(n);progress.restart();
            if(submitted==bytes.size()){queue.pop();submitted=confirmed=0;update();}
        }
        if(writable)writable->setEnabled(false);
    }
    void initialize(){
        progress.start();timer.setInterval(50);
        connect(&timer,&QTimer::timeout,this,[this]{if(!queue.empty()&&progress.elapsed()>=qint64(WayWinPolicy::StallMs))fail();else pump();});timer.start();
    }
public:
    std::function<void(bool)> flow;
    std::function<void(bool)> load;
    std::function<void()> fault;
    std::function<void(uint32_t)> superseded;
    explicit CommandWriter(QProcess *p,QObject *parent=nullptr):QObject(parent),process(p){
        initialize();
        connect(p,&QProcess::started,this,[this]{pump();});
        connect(p,&QProcess::bytesWritten,this,[this](qint64 n){
            if(queue.empty()||!submitted)return;
            confirmed+=size_t(n);progress.restart();
            if(confirmed>=queue.front().bytes.size()){queue.pop();submitted=confirmed=0;update();}
            pump();
        });
    }
    explicit CommandWriter(int descriptor,QObject *parent=nullptr):QObject(parent),fd(descriptor){
        ::fcntl(fd,F_SETFL,::fcntl(fd,F_GETFL)|O_NONBLOCK);
        writable=new QSocketNotifier(fd,QSocketNotifier::Write,this);writable->setEnabled(false);
        connect(writable,&QSocketNotifier::activated,this,[this]{pump();});initialize();
    }
    bool enqueue(WayWinPolicy::Packet packet){
        if(dead)return false;
        if(queue.empty())progress.restart();
        auto result=queue.push(std::move(packet),submitted!=0);
        if(result==WayWinPolicy::Admission::ControlFull){fail();return false;}
        if(result==WayWinPolicy::Admission::Full){if(!blocked){blocked=true;if(flow)flow(true);}return false;}
        if(queue.replacedRequest&&superseded)superseded(queue.replacedRequest);
        update();pump();return true;
    }
    bool low()const{return queue.low();}
    size_t bytes()const{return queue.bytes();}
    void discardPending(){queue.discardPending(submitted!=0);update();}
    void reset(){queue.clear();submitted=confirmed=0;dead=blocked=false;wasLow=true;progress.restart();}
};
}
