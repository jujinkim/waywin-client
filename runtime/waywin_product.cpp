// SPDX-License-Identifier: Apache-2.0
#include "waywin_product.hpp"
#include "sdl_context.hpp"
#include "waywin_policy.hpp"
#include <freerdp/dvc.h>
#include <freerdp/client/channels.h>
#include <freerdp/error.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <map>
#include <sstream>
#include <random>
#include <chrono>
#include <unistd.h>
using namespace WayWinWire;
std::atomic<bool> waywinRecovering{false};
std::atomic<unsigned> waywinEpoch{100u+(std::random_device{}()&0x3fffffffu)};
static std::atomic<unsigned> retryAction{0},retryAttempt{0},retrySeconds{0};
static std::atomic<Uint64> recoveryStart{0};
static std::atomic<bool> readerStarted{false},retryPaused{false};
static std::mutex commandsLock,channelLock,outputLock;
struct Command {unsigned action,window,icon,epoch;std::string payload;};
static WayWinPolicy::Queue commands;
static WayWinPolicy::Stall commandStall;
static bool commandsLow=true;
static std::atomic<bool> queueFatal{false};
static IWTSVirtualChannel* activeChannel=nullptr;
struct Picture {SDL_Window*window;SDL_Texture*texture;};
static std::map<SDL_Renderer*,Picture> pictures;
void waywinEmit(Message m) {
    if(!std::getenv("WAYWIN_PRODUCT"))return;
    if(!m.epoch)m.epoch=waywinEpoch.load();
    auto line=std::string("WAYWIN_UI_V2 ")+hex(encode(m))+"\n";
    std::lock_guard<std::mutex>lock(outputLock);
    // One stdio lock prevents interleaving with FreeRDP diagnostics.
    std::fwrite(line.data(),1,line.size(),stderr);std::fflush(stderr);
}
struct Callback {IWTSVirtualChannelCallback iface{};IWTSVirtualChannel*channel=nullptr;Decoder decoder;unsigned epoch=0;};
static UINT received(IWTSVirtualChannelCallback* base,wStream* stream) {
    auto*c=reinterpret_cast<Callback*>(base);
    try {for(auto m:c->decoder.feed(static_cast<const uint8_t*>(Stream_ConstPointer(stream)),Stream_GetRemainingLength(stream))) {
        if(m.type<Ready||m.type>InputAck||m.epoch!=c->epoch)continue;
        waywinEmit(std::move(m));
    }}catch(...){return ERROR_INVALID_DATA;}
    return CHANNEL_RC_OK;
}
static UINT opened(IWTSVirtualChannelCallback*base) {
    auto*c=reinterpret_cast<Callback*>(base);
    {std::lock_guard<std::mutex>lock(channelLock);activeChannel=c->channel;c->epoch=++waywinEpoch;}
    Message state{Channel,0,c->epoch,{}};put(state.data,1);waywinEmit(std::move(state));
    Message request{Hello,1,c->epoch,{}};put(request.data,RequestedCapabilities);auto hello=encode(request);
    return c->channel->Write(c->channel,static_cast<ULONG>(hello.size()),hello.data(),nullptr);
}
static UINT closed(IWTSVirtualChannelCallback*base) {
    auto*c=reinterpret_cast<Callback*>(base);
    {std::lock_guard<std::mutex>lock(channelLock);if(activeChannel==c->channel)activeChannel=nullptr;}
    Message state{Channel,0,c->epoch,{}};put(state.data,0);waywinEmit(std::move(state));delete c;return CHANNEL_RC_OK;
}
struct Plugin {IWTSPlugin iface{};IWTSListenerCallback listener{};};
static UINT newChannel(IWTSListenerCallback*,IWTSVirtualChannel*channel,BYTE*,BOOL*accept,IWTSVirtualChannelCallback**out) {
    auto*c=new Callback;c->channel=channel;c->iface.OnDataReceived=received;c->iface.OnOpen=opened;c->iface.OnClose=closed;
    *accept=TRUE;*out=&c->iface;return CHANNEL_RC_OK;
}
static UINT initialize(IWTSPlugin*base,IWTSVirtualChannelManager*manager) {
    auto*p=reinterpret_cast<Plugin*>(base);p->listener.OnNewChannelConnection=newChannel;
    IWTSListener*listener=nullptr;return manager->CreateListener(manager,"WayWin",0,&p->listener,&listener);
}
static UINT terminate(IWTSPlugin*base){delete reinterpret_cast<Plugin*>(base);return CHANNEL_RC_OK;}
static UINT VCAPITYPE entry(IDRDYNVC_ENTRY_POINTS*points) {
    auto*p=new Plugin;p->iface.Initialize=initialize;p->iface.Terminated=terminate;
    UINT rc=points->RegisterPlugin(points,"waywin",&p->iface);if(rc!=CHANNEL_RC_OK)delete p;return rc;
}
static PVIRTUALCHANNELENTRY provider(LPCSTR name,LPCSTR subsystem,LPCSTR type,DWORD flags) {
    if(name&&std::strcmp(name,"waywin")==0) {
        // FreeRDP's provider ABI uses a static-channel function pointer for DVC entries.
        PDVC_PLUGIN_ENTRY dvc=entry;PVIRTUALCHANNELENTRY out=nullptr;
        static_assert(sizeof(out)==sizeof(dvc));std::memcpy(&out,&dvc,sizeof(out));return out;
    }
    return freerdp_channels_load_static_addin_entry(name,subsystem,type,flags);
}
void waywinRegister(){if(std::getenv("WAYWIN_PRODUCT"))std::ignore=freerdp_register_addin_provider(provider,0);}
static void queueFlow(unsigned state) {
    Message m{Flow,0,waywinEpoch.load(),{}};put(m.data,2);put(m.data,state);waywinEmit(std::move(m));
}
void waywinStartCommands() {
    if(!std::getenv("WAYWIN_PRODUCT")||readerStarted.exchange(true))return;
    std::thread([]{
        std::string line;char c=0;
        while(::read(STDIN_FILENO,&c,1)==1) {
            if(c=='\n') {
                try {
                    auto packet=WayWinPolicy::commandPacket(line);
                    std::istringstream input(line);Command cmd{};input>>cmd.action>>cmd.window>>cmd.icon>>cmd.epoch>>cmd.payload;
                    if(cmd.epoch==waywinEpoch) {
                        if(cmd.action==23){unsigned idle=0;retryAction.compare_exchange_strong(idle,23);}
                        else if(cmd.action==21||cmd.action==22)retryAction=cmd.action;
                        else {
                            WayWinPolicy::Admission admitted;uint32_t replaced=0;bool notify=false,loadChanged=false;
                            {std::lock_guard<std::mutex>lock(commandsLock);
                                admitted=commands.push(std::move(packet));replaced=commands.replacedRequest;loadChanged=commandsLow&&!commands.low();commandsLow=commands.low();
                                if(admitted!=WayWinPolicy::Admission::Accepted||commands.high()){notify=!commandStall.active();commandStall.set(true,SDL_GetTicks());}
                                // Serialize watermark notifications with queue changes so a
                                // late blocked event cannot overtake a subsequent resume.
                                if(admitted==WayWinPolicy::Admission::ControlFull){queueFatal=true;queueFlow(2);}
                                else if(notify)queueFlow(1);
                                else if(loadChanged)queueFlow(3);
                            }
                            if(replaced)waywinEmit({InputAck,replaced,cmd.epoch,{}});
                        }
                    }
                }catch(...){queueFatal=true;queueFlow(2);}
                line.clear();
            }else if(line.size()<WayWinPolicy::CommandLineBytes)line+=c;else {queueFatal=true;queueFlow(2);break;}
            bool expired=false;{std::lock_guard<std::mutex>lock(commandsLock);expired=commandStall.expired(SDL_GetTicks());}
            if(expired){queueFatal=true;queueFlow(2);break;}
        }
    }).detach();
}
void waywinTrack(SDL_Window*w,SDL_Renderer*r,SDL_Texture*t){
    if(!std::getenv("WAYWIN_PRODUCT"))return;
    pictures[r]={w,t};unsigned color=0x7651c9;
    if(const char*accent=std::getenv("WAYWIN_ACCENT")){if(std::strlen(accent)==7&&accent[0]=='#')color=static_cast<unsigned>(std::strtoul(accent+1,nullptr,16));}
    int width=0,height=0;SDL_GetRenderOutputSize(r,&width,&height);
    SDL_FRect stripe{0,0,static_cast<float>(width),3};
    std::ignore=SDL_SetRenderDrawColor(r,static_cast<Uint8>(color>>16),static_cast<Uint8>(color>>8),static_cast<Uint8>(color),255);
    std::ignore=SDL_RenderFillRect(r,&stripe);
}
void waywinForget(SDL_Renderer*r){pictures.erase(r);}
static void retryStatus(unsigned,unsigned,unsigned,unsigned);
static void overlay() {
    if(!waywinRecovering||SDL_GetTicks()-recoveryStart<500)return;
    for(auto&[renderer,picture]:pictures) {
        if(!picture.window||!picture.texture)continue;
        int w=0,h=0;SDL_GetRenderOutputSize(renderer,&w,&h);
        std::ignore=SDL_SetRenderTarget(renderer,nullptr);
        std::ignore=SDL_RenderTexture(renderer,picture.texture,nullptr,nullptr);
        std::ignore=SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
        std::ignore=SDL_SetRenderDrawColor(renderer,0,0,0,170);
        std::ignore=SDL_RenderFillRect(renderer,nullptr);
        if(w>=240) {
            std::ignore=SDL_SetRenderDrawColor(renderer,255,255,255,255);
            const auto title=SDL_GetWindowTitle(picture.window);
            const std::string text=std::string(title?title:"WayWin").substr(0,80)+"\nConnection interrupted\nAttempt "+std::to_string(retryAttempt.load())+"/5  wait "+std::to_string(retrySeconds.load())+"s";
            const float x=static_cast<float>(std::max(12,(w-220)/2)),y=static_cast<float>(std::max(12,(h-100)/2));
            std::ignore=SDL_RenderDebugText(renderer,x,y,text.c_str());
            SDL_FRect button{x,y+60,208,28};
            std::ignore=SDL_SetRenderDrawColor(renderer,75,60,135,255);std::ignore=SDL_RenderFillRect(renderer,&button);
            std::ignore=SDL_SetRenderDrawColor(renderer,255,255,255,255);
            std::ignore=SDL_RenderDebugText(renderer,x+12,y+70,retryPaused?"Retry connection":"Cancel retries");
        }
        std::ignore=SDL_RenderPresent(renderer);
    }
}
bool waywinInput(const SDL_Event&ev) {
    if(!waywinRecovering)return false;
    if(ev.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED){retryStatus(4,retryAttempt,0,0);return true;}
    if(ev.type==SDL_EVENT_MOUSE_BUTTON_UP){
        auto window=SDL_GetWindowFromID(ev.button.windowID);int w=0,h=0,lw=0,lh=0;
        if(window){SDL_GetWindowSizeInPixels(window,&w,&h);SDL_GetWindowSize(window,&lw,&lh);}
        const float px=ev.button.x*static_cast<float>(w)/static_cast<float>(std::max(1,lw));
        const float py=ev.button.y*static_cast<float>(h)/static_cast<float>(std::max(1,lh));
        const int x=std::max(12,(w-220)/2),y=std::max(12,(h-100)/2)+60;
        if(w>=240&&px>=x&&px<x+208&&py>=y&&py<y+28)retryAction=retryPaused?21:22;
        return true;
    }
    return (ev.type>=SDL_EVENT_KEY_DOWN&&ev.type<=SDL_EVENT_TEXT_EDITING_CANDIDATES)||
        (ev.type>=SDL_EVENT_MOUSE_MOTION&&ev.type<=SDL_EVENT_MOUSE_WHEEL)||
        (ev.type>=SDL_EVENT_FINGER_DOWN&&ev.type<=SDL_EVENT_FINGER_CANCELED);
}
void waywinTick(SdlContext*sdl) {
    if(!std::getenv("WAYWIN_PRODUCT"))return;
    if(queueFatal)return;
    for(;;) {
        std::string line;bool resumed=false;uint32_t completion=0;
        {std::lock_guard<std::mutex>lock(commandsLock);
            if(commands.empty())break;
            completion=commands.front().request;line=std::move(commands.front().bytes);commands.pop();
            if(commands.low()&&(commandStall.active()||!commandsLow)){commandStall.set(false,SDL_GetTicks());resumed=true;}commandsLow=commands.low();
            if(resumed)queueFlow(0);
        }
        Command c{};std::istringstream input(line);input>>c.action>>c.window>>c.icon>>c.epoch>>c.payload;
        if(c.epoch!=waywinEpoch||waywinRecovering)continue;
        if(c.action>=1&&c.action<=6){sdl->getRailChannelContext().productCommand(c.action,c.window,c.icon);if(completion)waywinEmit({InputAck,completion,c.epoch,{}});}
        else if(c.action==20)try {
            auto bytes=unhex(c.payload);Decoder decoder;auto messages=decoder.feed(bytes.data(),bytes.size());
            if(messages.size()!=1||messages[0].epoch!=waywinEpoch)continue;
            std::lock_guard<std::mutex>lock(channelLock);
            if(activeChannel)std::ignore=activeChannel->Write(activeChannel,static_cast<ULONG>(bytes.size()),bytes.data(),nullptr);
        }catch(...){}
    }
    sdl->getRailChannelContext().productTick();
    overlay();
}
static bool networkFailure(freerdp*instance) {
    const auto info=freerdp_error_info(instance),error=freerdp_get_last_error(instance->context);
    if(info!=ERRINFO_SUCCESS&&info!=ERRINFO_GRAPHICS_SUBSYSTEM_FAILED)return false;
    return error==FREERDP_ERROR_SUCCESS||error==FREERDP_ERROR_CONNECT_TRANSPORT_FAILED||
        error==FREERDP_ERROR_CONNECT_FAILED||error==FREERDP_ERROR_DNS_NAME_NOT_FOUND||
        error==FREERDP_ERROR_DNS_ERROR;
}
static void retryStatus(unsigned state,unsigned attempt,unsigned seconds,unsigned error) {
    static std::mutex statusLock;static unsigned previous[4]={~0u,~0u,~0u,~0u};
    std::lock_guard<std::mutex>guard(statusLock);
    if(previous[0]==state&&previous[1]==attempt&&previous[2]==seconds&&previous[3]==error)return;
    previous[0]=state;previous[1]=attempt;previous[2]=seconds;previous[3]=error;
    Message m{Retry,0,waywinEpoch.load(),{}};put(m.data,state);put(m.data,attempt);put(m.data,seconds);put(m.data,error);waywinEmit(std::move(m));
}
bool waywinReconnect(freerdp*instance) {
    if(!std::getenv("WAYWIN_PRODUCT"))return client_auto_reconnect(instance);
    if(!networkFailure(instance))return false;
    waywinRecovering=true;retryPaused=false;recoveryStart=SDL_GetTicks();retryAction=0;++waywinEpoch;
    // Never replay RemoteApplicationProgram after the initial connection.
    std::ignore=freerdp_settings_set_string(instance->context->settings,FreeRDP_RemoteApplicationProgram,"");
    std::mt19937 random(std::random_device{}());
    WayWinPolicy::RetryBudget budget;
    auto delay=[&](unsigned attempt){return (1u<<attempt)*static_cast<unsigned>(std::uniform_int_distribution<int>(800,1200)(random));};
    budget.disconnected(SDL_GetTicks(),delay(0));
    for(;;) {
        const auto action=retryAction.exchange(0);const auto now=SDL_GetTicks();
        if(action==22)budget.stop();
        else if(action==21)budget.manual(now);
        else if(action==23)budget.wake(now);
        retryPaused=budget.state!=WayWinPolicy::RetryState::Automatic;
        retryAttempt=budget.attempts;
        if(freerdp_get_last_error(instance->context)==FREERDP_ERROR_CONNECT_CANCELLED){waywinRecovering=false;return false;}
        if(retryPaused){retrySeconds=0;retryStatus(budget.state==WayWinPolicy::RetryState::Stopped?2:5,budget.attempts,0,freerdp_get_last_error(instance->context));std::this_thread::sleep_for(std::chrono::milliseconds(100));continue;}
        if(now<budget.due){retrySeconds=static_cast<unsigned>((budget.due-now+999)/1000);retryStatus(1,budget.attempts,retrySeconds,0);std::this_thread::sleep_for(std::chrono::milliseconds(100));continue;}
        if(!budget.begin(now))continue;
        retryAttempt=budget.attempts;retrySeconds=0;retryStatus(1,budget.attempts,0,0);
        std::mutex lock;std::condition_variable done;bool finished=false;std::atomic<bool> timedOut{false};
        std::thread watchdog([&]{std::unique_lock<std::mutex>guard(lock);if(!done.wait_for(guard,std::chrono::seconds(9),[&]{return finished;})){timedOut=true;std::ignore=freerdp_abort_connect_context(instance->context);}});
        const bool connected=freerdp_reconnect(instance);
        {std::lock_guard<std::mutex>guard(lock);finished=true;}done.notify_one();watchdog.join();
        if(connected){{std::lock_guard<std::mutex>guard(commandsLock);commands.clear();commandsLow=true;commandStall.set(false,SDL_GetTicks());}budget.succeeded();waywinRecovering=false;retryStatus(0,retryAttempt,0,0);return true;}
        if(timedOut)freerdp_set_last_error(instance->context,FREERDP_ERROR_CONNECT_TRANSPORT_FAILED);
        if(!networkFailure(instance)){budget.forbid();retryStatus(3,budget.attempts,0,freerdp_get_last_error(instance->context));waywinRecovering=false;return false;}
        budget.failed(SDL_GetTicks(),delay(budget.attempts));
    }
}
