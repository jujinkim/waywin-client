// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "waywin_wire.hpp"
#include <deque>
#include <map>
#include <cstddef>
#include <utility>
#include <sstream>
namespace WayWinPolicy {
constexpr size_t IdLimit=1024, ImageBytes=16*1024*1024;
constexpr size_t NormalBytes=256*1024, NormalCommands=128;
constexpr size_t ControlBytes=64*1024, ControlCommands=32, ResumeBytes=128*1024;
constexpr size_t PendingRequests=64;
constexpr uint64_t StallMs=5000, GraceMs=3000, CapturePixels=16777216;
constexpr size_t CaptureMemory=256*1024*1024;
inline bool input(uint32_t type) {return type==WayWinWire::Pointer||type==WayWinWire::Key||type==WayWinWire::Text;}
struct Packet {
    std::string bytes;
    bool control=false, motion=false;
    uint32_t target=0, epoch=0, request=0;
};
inline Packet packet(std::string bytes,const WayWinWire::Message&m) {
    Packet p{std::move(bytes),m.type==WayWinWire::SurfaceClose||m.type==WayWinWire::SurfaceState,false,0,m.epoch,m.request};
    if(m.data.size()>=4)p.target=WayWinWire::get(m.data.data());
    if(m.type==WayWinWire::Key&&m.data.size()==12)p.control=WayWinWire::get(m.data.data()+8)==0;
    if(m.type==WayWinWire::Pointer&&m.data.size()==16){auto action=WayWinWire::get(m.data.data()+12);p.control=action==2||action==4;p.motion=action==0;}
    return p;
}
constexpr size_t CommandLineBytes=2*(65552+24)+128;
inline Packet commandPacket(const std::string&line) {
    if(line.size()>CommandLineBytes)throw std::runtime_error("command limit");
    unsigned action=0,window=0,icon=0,epoch=0;std::string payload,extra;
    std::istringstream stream(line);
    if(!(stream>>action>>window>>icon>>epoch>>payload)||(stream>>extra))throw std::runtime_error("invalid command");
    if(action==20){auto bytes=WayWinWire::unhex(payload);WayWinWire::Decoder decoder;auto messages=decoder.feed(bytes.data(),bytes.size());
        if(messages.size()!=1||WayWinWire::encode(messages[0])!=bytes||!WayWinWire::requestType(messages[0].type)||messages[0].epoch!=epoch||messages[0].data.size()>65552)throw std::runtime_error("invalid request");
        return packet(line+"\n",messages[0]);
    }
    if(!((action>=1&&action<=6)||(action>=21&&action<=23)))throw std::runtime_error("invalid action");
    uint32_t request=0;
    if(payload!="-"){
        if(action>=21||!std::all_of(payload.begin(),payload.end(),[](char c){return c>='0'&&c<='9';}))throw std::runtime_error("invalid completion ID");
        const auto value=std::stoull(payload);if(!value||value>0xffffffffu)throw std::runtime_error("invalid completion ID");request=uint32_t(value);
    }
    return {line+"\n",action==4||action>=21,false,window,epoch,request};
}
enum class Admission { Accepted, Full, ControlFull };
// FIFO, including the control reserve: a release can never overtake a press.
class Queue {
    std::deque<Packet> items;
    size_t ordinaryBytes=0,reservedBytes=0,ordinaryCount=0,reservedCount=0;
    void subtract(const Packet&p){if(p.control){reservedBytes-=p.bytes.size();--reservedCount;}else{ordinaryBytes-=p.bytes.size();--ordinaryCount;}}
public:
    uint32_t replacedRequest=0;
    Admission push(Packet p,bool protectFront=false) {
        replacedRequest=0;
        if(p.motion) {
            for(auto it=items.rbegin();it!=items.rend();++it) {
                if(it->epoch!=p.epoch)break;
                if(it->target!=p.target)continue;
                if(!it->motion||(protectFront && &*it==&items.front()))break;
                if(ordinaryBytes-it->bytes.size()+p.bytes.size()>NormalBytes)return Admission::Full;
                ordinaryBytes=ordinaryBytes-it->bytes.size()+p.bytes.size();replacedRequest=it->request;*it=std::move(p);return Admission::Accepted;
            }
        }
        if(p.control){if(reservedBytes+p.bytes.size()>ControlBytes||reservedCount>=ControlCommands)return Admission::ControlFull;reservedBytes+=p.bytes.size();++reservedCount;}
        else {if(ordinaryBytes+p.bytes.size()>NormalBytes||ordinaryCount>=NormalCommands)return Admission::Full;ordinaryBytes+=p.bytes.size();++ordinaryCount;}
        items.push_back(std::move(p));return Admission::Accepted;
    }
    bool empty()const{return items.empty();}
    size_t size()const{return items.size();}
    size_t bytes()const{return ordinaryBytes+reservedBytes;}
    size_t normalBytes()const{return ordinaryBytes;}
    bool high()const{return ordinaryBytes>=NormalBytes||ordinaryCount>=NormalCommands;}
    bool low()const{return bytes()<ResumeBytes&&ordinaryCount<NormalCommands;}
    Packet& front(){return items.front();}
    void pop(){subtract(items.front());items.pop_front();}
    void clear(){items.clear();ordinaryBytes=reservedBytes=ordinaryCount=reservedCount=0;}
    void discardPending(bool keepFront){if(!keepFront){clear();return;}while(items.size()>1){subtract(items.back());items.pop_back();}}
};
class Stall {
    bool blocked=false;
    uint64_t since=0;
public:
    void set(bool value,uint64_t now){if(value&&!blocked)since=now;blocked=value;}
    bool active()const{return blocked;}
    bool expired(uint64_t now)const{return blocked&&now-since>=StallMs;}
};
enum class RetryState { Automatic, Stopped, Exhausted, Forbidden };
class RetryBudget {
public:
    RetryState state=RetryState::Automatic;
    unsigned attempts=0;
    uint64_t due=0;
    void disconnected(uint64_t now,unsigned delay){state=RetryState::Automatic;attempts=0;due=now+delay;}
    void stop(){if(state!=RetryState::Forbidden)state=RetryState::Stopped;}
    void forbid(){state=RetryState::Forbidden;}
    void manual(uint64_t now){if(state!=RetryState::Forbidden){attempts=0;state=RetryState::Automatic;due=now;}}
    void wake(uint64_t now){if(state==RetryState::Automatic)due=now;}
    bool begin(uint64_t now){if(state!=RetryState::Automatic||now<due)return false;if(attempts>=5){state=RetryState::Exhausted;return false;}++attempts;return true;}
    void failed(uint64_t now,unsigned delay){if(attempts>=5)state=RetryState::Exhausted;else due=now+delay;}
    void succeeded(){attempts=0;state=RetryState::Automatic;due=0;}
};
// One global 250 ms slot. Slots are discarded when late; no accumulated work.
class CaptureScheduler {
    struct Target {uint32_t state;bool first=true;};
    std::map<uint32_t,Target> targets;
    uint64_t next=0;
    uint32_t cursor=0,backgroundCursor=0;
    unsigned phase=0;
public:
    void add(uint32_t id){targets.emplace(id,Target{WayWinWire::Background,true});}
    void remove(uint32_t id){targets.erase(id);}
    void clear(){targets.clear();next=0;cursor=backgroundCursor=0;phase=0;}
    void state(uint32_t id,uint32_t value){auto it=targets.find(id);if(it==targets.end()||value>WayWinWire::Active)return;if(value==WayWinWire::Active)for(auto&entry:targets)if(entry.second.state==WayWinWire::Active)entry.second.state=WayWinWire::Background;it->second.state=value;}
    uint32_t take(uint64_t now,bool priority) {
        if(now<next)return 0;
        bool active=false,background=false;
        for(const auto&entry:targets){active|=entry.second.state==WayWinWire::Active;background|=entry.second.state==WayWinWire::Background;}
        if(!active&&!background)return 0;
        uint32_t wanted=WayWinWire::Background;
        if(priority&&active){wanted=background&&phase==3?WayWinWire::Background:WayWinWire::Active;next=now+250;phase=(phase+1)%4;}
        else {next=now+(priority?1000:250);phase=0;}
        // First frames get the next slot, including while the active window is busy.
        for(auto&entry:targets)if(entry.second.first&&entry.second.state!=WayWinWire::Hidden){entry.second.first=false;if(entry.second.state==WayWinWire::Background)backgroundCursor=entry.first;cursor=entry.first;return cursor;}
        auto eligible=[&](const auto&e){return priority?e.second.state==wanted:e.second.state!=WayWinWire::Hidden;};
        auto it=targets.upper_bound(priority?backgroundCursor:cursor);
        for(size_t i=0;i<targets.size();++i){if(it==targets.end())it=targets.begin();if(eligible(*it)){cursor=it->first;if(priority&&wanted==WayWinWire::Background)backgroundCursor=cursor;return cursor;}++it;}
        return 0;
    }
};
class CaptureDeadline {
    bool working=false,disabled=false;
    uint64_t since=0;
public:
    bool available()const{return !working&&!disabled;}
    bool busy()const{return working;}
    bool start(uint64_t now){if(!available())return false;working=true;since=now;return true;}
    void complete(){working=false;}
    bool expired(uint64_t now)const{return working&&now-since>=StallMs;}
    void fault(){working=false;disabled=true;}
    void reopen(){disabled=false;}
};
}
