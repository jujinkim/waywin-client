// SPDX-License-Identifier: Apache-2.0
#include <windows.h>
#include <wtsapi32.h>
#include <pchannel.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <filesystem>
#include <mutex>
#include <deque>
#include <atomic>
#include <thread>
#include <map>
#include <set>
#include <memory>
#include "waywin_wire.hpp"
#include "capture_worker.h"
#include "waywin_policy.hpp"
using namespace WayWinWire;
namespace fs=std::filesystem;
static HWND mainWindow=nullptr,statusLabel=nullptr;
static DWORD sessionId=0;
static std::string identity;
static std::atomic<bool> running{true},linked{false};
static std::mutex queueLock;
static WayWinPolicy::Queue inbound;
static std::deque<Message> outbound;
static std::map<uint32_t,fs::path> apps;
static std::map<uint32_t,HWND> surfaces;
static uint32_t epoch=0,nextSurface=0;
static std::map<uint32_t,uint32_t> surfaceKinds;
static std::map<uint32_t,std::pair<uint32_t,ULONGLONG>> seeking;
static std::set<WORD> heldKeys;
static CaptureProcess captureProcess;
static WayWinPolicy::CaptureScheduler captureSchedule;
static std::atomic<uint32_t> negotiated{0};
static unsigned heldMouse=0;
static std::wstring wide(const std::string&s) {
    if(s.empty())return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);
    if(!n)throw std::runtime_error("invalid UTF-8");
    std::wstring w(size_t(n),0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),w.data(),n);return w;
}
static std::string utf8(const std::wstring&w) {
    int n=WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);
    std::string s(size_t(n),0);WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),s.data(),n,nullptr,nullptr);return s;
}
static void send(Message m) {
    std::lock_guard<std::mutex>lock(queueLock);
    if(!linked)return;
    if(m.type==Frame) {for(auto it=outbound.begin();it!=outbound.end();)if(it->type==Frame&&it->request==m.request)it=outbound.erase(it);else ++it;}
    if(outbound.size()<64)outbound.push_back(std::move(m));
}
static void result(const Message&m,uint32_t code,const std::string&text) {
    Message r{Result,m.request,m.epoch,{}};put(r.data,code);string(r.data,text);send(std::move(r));
}
static void acknowledge(const Message&m) {
    if((negotiated&InputCompletion)&&(WayWinPolicy::input(m.type)||m.type==SurfaceClose||m.type==SurfaceState))send({InputAck,m.request,m.epoch,{}});
}
static void releaseKeys() {
    for(WORD key:heldKeys){INPUT i{};i.type=INPUT_KEYBOARD;i.ki.wVk=key;i.ki.dwFlags=KEYEVENTF_KEYUP;SendInput(1,&i,sizeof(i));}
    heldKeys.clear();
    if(heldMouse){INPUT mouse{};mouse.type=INPUT_MOUSE;mouse.mi.dwFlags=MOUSEEVENTF_LEFTUP|MOUSEEVENTF_RIGHTUP|MOUSEEVENTF_MIDDLEUP;SendInput(1,&mouse,sizeof(mouse));heldMouse=0;}
}
static void keyStroke(WORD key,bool down) {
    INPUT i{};i.type=INPUT_KEYBOARD;i.ki.wVk=key;i.ki.dwFlags=down?0:KEYEVENTF_KEYUP;
    if(SendInput(1,&i,sizeof(i))!=1)throw std::runtime_error("Windows blocked input; check foreground desktop and integrity level.");
    if(down)heldKeys.insert(key);else heldKeys.erase(key);
}
static void enumerateApps(const Message&m) {
    apps.clear();uint32_t id=0;Message r{AppList,m.request,m.epoch,{}};put(r.data,0);
    for(const KNOWNFOLDERID* folder:{&FOLDERID_Programs,&FOLDERID_CommonPrograms}) {
        PWSTR base=nullptr;if(FAILED(SHGetKnownFolderPath(*folder,0,nullptr,&base)))continue;
        std::error_code error;
        for(fs::recursive_directory_iterator it(base,fs::directory_options::skip_permission_denied,error),end;it!=end&&!error;it.increment(error)) {
            if(id>=1000)break;
            if(it->is_symlink(error)){if(it->is_directory(error))it.disable_recursion_pending();continue;}
            if(!it->is_regular_file(error))continue;
            auto ext=it->path().extension().wstring();std::transform(ext.begin(),ext.end(),ext.begin(),towlower);
            if(ext!=L".lnk"&&ext!=L".appref-ms")continue;
            apps[++id]=it->path();put(r.data,id);string(r.data,utf8(it->path().stem().wstring()));
        }
        CoTaskMemFree(base);
    }
    for(int i=0;i<4;++i)r.data[size_t(i)]=uint8_t(id>>(i*8));
    send(std::move(r));
}
static void handle(const Message&m) {
    if(!requestType(m.type)||!m.request)return;
    if(m.type==Hello){
        Reader read(m.data);auto requested=read.done()?0:read.number();if(!read.done())throw std::runtime_error("Invalid capabilities");
        if(epoch!=m.epoch){releaseKeys();surfaces.clear();surfaceKinds.clear();seeking.clear();captureSchedule.clear();captureProcess.stop();}
        epoch=m.epoch;negotiated=requested&RequestedCapabilities;
        Message r{Ready,m.request,m.epoch,{}};put(r.data,sessionId);string(r.data,identity);put(r.data,0x3f|RequestedCapabilities);send(std::move(r));return;
    }
    if(m.epoch!=epoch){result(m,2,"Stale channel generation");return;}
    Reader read(m.data);
    if(m.type==Apps){if(!read.done())throw std::runtime_error("invalid list request");enumerateApps(m);return;}
    if(m.type==Launch){
        auto id=read.number();if(!read.done()||!apps.count(id))throw std::runtime_error("Refresh the application list before launching.");
        auto rc=reinterpret_cast<INT_PTR>(ShellExecuteW(mainWindow,L"open",apps.at(id).c_str(),nullptr,nullptr,SW_SHOWNORMAL));
        result(m,rc>32?0:1,rc>32?"Application launch requested":"Windows refused application launch");return;
    }
    if(m.type==WayWinWire::Shell) {
        uint32_t kind=read.number();if(!read.done()||kind<1||kind>4)throw std::runtime_error("Unknown shell surface");
        releaseKeys();captureProcess.reopen();
        if(kind==2) {
            if(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",L"ms-settings:regionlanguage",nullptr,nullptr,SW_SHOWNORMAL))<=32)throw std::runtime_error("Windows settings unavailable");
        } else if(kind!=4) {
            keyStroke(VK_LWIN,true);if(kind==3)keyStroke('A',true);
            if(kind==3)keyStroke('A',false);
            keyStroke(VK_LWIN,false);
        }
        seeking[m.request]={kind,GetTickCount64()+5000};return;
    }
    if(m.type==ClipboardGet) {
        if(!read.done()||!OpenClipboard(mainWindow))throw std::runtime_error("Clipboard is busy");
        HANDLE h=GetClipboardData(CF_UNICODETEXT);std::wstring value;
        if(h){auto p=static_cast<const wchar_t*>(GlobalLock(h));if(p){auto size=GlobalSize(h)/sizeof(wchar_t);size=std::min<size_t>(size,32769);auto end=std::find(p,p+size,L'\0');if(end!=p+size)value.assign(p,end);GlobalUnlock(h);}}
        CloseClipboard();auto text=utf8(value);if(text.size()>65536)throw std::runtime_error("Text exceeds 64 KiB");
        Message r{Clipboard,m.request,m.epoch,{}};string(r.data,text);send(std::move(r));return;
    }
    if(m.type==ClipboardSet) {
        auto value=wide(read.text());if(!read.done())throw std::runtime_error("Invalid clipboard request");
        HGLOBAL mem=GlobalAlloc(GMEM_MOVEABLE,(value.size()+1)*sizeof(wchar_t));if(!mem)throw std::runtime_error("Clipboard allocation failed");
        void*p=GlobalLock(mem);if(!p){GlobalFree(mem);throw std::runtime_error("Clipboard allocation failed");}
        memcpy(p,value.c_str(),(value.size()+1)*sizeof(wchar_t));GlobalUnlock(mem);
        if(!OpenClipboard(mainWindow)){GlobalFree(mem);throw std::runtime_error("Clipboard is busy");}
        bool ok=EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,mem);CloseClipboard();if(!ok)GlobalFree(mem);
        result(m,ok?0:1,ok?"Text sent to this Windows session":"Clipboard update failed");return;
    }
    auto id=read.number();
    if(m.type==SurfaceState){
        auto value=read.number();if(!read.done()||value>Active||!surfaces.count(id)||!(negotiated&SurfacePriority))throw std::runtime_error("Invalid surface state");
        captureSchedule.state(id,value);acknowledge(m);return;
    }
    if(!surfaces.count(id)||!shellAllowed(surfaces[id],surfaceKinds[id]))throw std::runtime_error("Shell surface closed or inaccessible");
    HWND w=surfaces[id];
    if(m.type==SurfaceClose){if(!read.done())throw std::runtime_error("Invalid close request");releaseKeys();if(surfaceKinds[id]==2)PostMessageW(w,WM_CLOSE,0,0);else if(surfaceKinds[id]==4||SetForegroundWindow(w)){keyStroke(VK_ESCAPE,true);keyStroke(VK_ESCAPE,false);}surfaces.erase(id);surfaceKinds.erase(id);captureSchedule.remove(id);send({SurfaceGone,id,m.epoch,{}});acknowledge(m);return;}
    if(surfaceKinds[id]!=4&&GetAncestor(GetForegroundWindow(),GA_ROOT)!=w&&!SetForegroundWindow(w))throw std::runtime_error("Windows denied shell focus; reopen the surface.");
    if(m.type==Pointer) {
        auto x=read.number(),y=read.number(),action=read.number();if(!read.done()||x>65535||y>65535||action>7)throw std::runtime_error("Invalid pointer request");
        RECT r{};GetWindowRect(w,&r);POINT p{r.left+LONG((uint64_t(x)*(r.right-r.left-1))/65535),r.top+LONG((uint64_t(y)*(r.bottom-r.top-1))/65535)};
        HWND hit=GetAncestor(WindowFromPoint(p),GA_ROOT);if(hit!=w)throw std::runtime_error("Shell surface is occluded; reopen it.");
        INPUT i{};i.type=INPUT_MOUSE;int vx=GetSystemMetrics(SM_XVIRTUALSCREEN),vy=GetSystemMetrics(SM_YVIRTUALSCREEN);
        i.mi.dx=LONG((int64_t(p.x-vx)*65535)/std::max(1,GetSystemMetrics(SM_CXVIRTUALSCREEN)-1));
        i.mi.dy=LONG((int64_t(p.y-vy)*65535)/std::max(1,GetSystemMetrics(SM_CYVIRTUALSCREEN)-1));
        i.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
        const DWORD flags[]={0,MOUSEEVENTF_LEFTDOWN,MOUSEEVENTF_LEFTUP,MOUSEEVENTF_RIGHTDOWN,MOUSEEVENTF_RIGHTUP,MOUSEEVENTF_WHEEL,MOUSEEVENTF_WHEEL,0};
        i.mi.dwFlags|=flags[action];if(action==5||action==6)i.mi.mouseData=DWORD(action==5?WHEEL_DELTA:-WHEEL_DELTA);
        if(SendInput(1,&i,sizeof(i))!=1)throw std::runtime_error("Windows denied pointer input");
        if(action==1)heldMouse|=1;
        if(action==2)heldMouse&=~1u;
        if(action==3)heldMouse|=2;
        if(action==4)heldMouse&=~2u;
    } else if(m.type==Key) {
        auto key=read.number(),down=read.number();if(!read.done()||key<8||key>0xfe||down>1)throw std::runtime_error("Invalid key");
        keyStroke(WORD(key),down!=0);
    } else if(m.type==Text) {
        auto value=wide(read.text(4096));if(!read.done())throw std::runtime_error("Invalid text");
        for(wchar_t c:value){INPUT i[2]{};i[0].type=i[1].type=INPUT_KEYBOARD;i[0].ki.wScan=i[1].ki.wScan=WORD(c);i[0].ki.dwFlags=KEYEVENTF_UNICODE;i[1].ki.dwFlags=KEYEVENTF_UNICODE|KEYEVENTF_KEYUP;if(SendInput(2,i,sizeof(INPUT))!=2)throw std::runtime_error("Windows denied text input");}
    } else throw std::runtime_error("Unsupported operation");
    acknowledge(m);
}
static void tick() {
    std::deque<Message> messages;
    {std::lock_guard<std::mutex>lock(queueLock);
        while(!inbound.empty()){const auto &bytes=inbound.front().bytes;Decoder decoder;auto decoded=decoder.feed(reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size());if(decoded.size()==1)messages.push_back(std::move(decoded[0]));inbound.pop();}
    }
    if(!linked){messages.clear();surfaces.clear();surfaceKinds.clear();seeking.clear();captureSchedule.clear();captureProcess.stop();releaseKeys();}
    for(const auto&m:messages)try{handle(m);}catch(const std::exception&e){if(WayWinPolicy::input(m.type))releaseKeys();result(m,1,e.what());}
    for(auto it=seeking.begin();it!=seeking.end();) {
        struct Search {uint32_t kind;HWND found;};Search find{it->second.first,nullptr};
        EnumWindows([](HWND w,LPARAM arg)->BOOL{auto&s=*reinterpret_cast<Search*>(arg);if(shellAllowed(w,s.kind)){s.found=w;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&find));
        if(find.found){uint32_t id=0;for(const auto&entry:surfaces)if(entry.second==find.found)id=entry.first;if(!id&&surfaces.size()>=8){Message m{WayWinWire::Shell,it->first,epoch,{}};result(m,1,"Close an existing shell surface first.");it=seeking.erase(it);continue;}if(!id)id=++nextSurface;surfaces[id]=find.found;surfaceKinds[id]=find.kind;captureSchedule.add(id);Message r{Result,it->first,epoch,{}};put(r.data,0);string(r.data,"Original Windows UI opened");send(std::move(r));it=seeking.erase(it);}
        else if(GetTickCount64()>it->second.second){Message m{WayWinWire::Shell,it->first,epoch,{}};result(m,1,"Windows did not expose this shell UI. Check edition, shell session, and supported RemoteApp configuration.");it=seeking.erase(it);}
        else ++it;
    }
    for(auto it=surfaces.begin();it!=surfaces.end();) {
        auto id=it->first;
        if(!shellAllowed(it->second,surfaceKinds[id])){releaseKeys();Message m{SurfaceGone,id,epoch,{}};send(std::move(m));surfaceKinds.erase(id);captureSchedule.remove(id);it=surfaces.erase(it);continue;}
        ++it;
    }
    auto captureFault=[](){
        releaseKeys();captureProcess.fault();
        for(const auto&entry:surfaces){Message m{SurfaceGone,entry.first,epoch,{}};string(m.data,"Windows shell capture stopped (timeout, process exit, or IPC error). Reopen the shell to retry.");send(std::move(m));}
        surfaces.clear();surfaceKinds.clear();seeking.clear();captureSchedule.clear();
    };
    Message frame;const auto now=GetTickCount64();const auto captured=captureProcess.poll(now,frame);
    if(captured<0)captureFault();
    else if(captured>0&&frame.epoch==epoch&&surfaces.count(frame.request)){
        if(frame.type==SurfaceGone){
            releaseKeys();string(frame.data,"Windows could not capture this shell surface. Reopen it to retry.");
            surfaces.erase(frame.request);surfaceKinds.erase(frame.request);captureSchedule.remove(frame.request);
        }
        send(std::move(frame));
    }
    if(captureProcess.available()){
        const auto id=captureSchedule.take(now,(negotiated&SurfacePriority)!=0);
        if(id&&surfaces.count(id)&&!captureProcess.submit(id,epoch,surfaceKinds.at(id),surfaces.at(id),now))captureFault();
    }
    SetWindowTextW(statusLabel,linked?L"Connected through the current RDP session. No extra network port.":L"Waiting for WayWin RDP connection. Sign into this Windows session first.");
}
static void channelLoop() {
    while(running) {
        HANDLE channel=WTSVirtualChannelOpenEx(sessionId,const_cast<char*>("WayWin"),WTS_CHANNEL_OPTION_DYNAMIC|WTS_CHANNEL_OPTION_DYNAMIC_PRI_LOW);
        if(!channel){for(int i=0;i<10&&running;++i)Sleep(100);continue;}
        Decoder decoder;linked=true;
        while(running) {
            std::deque<Message> pending;{std::lock_guard<std::mutex>lock(queueLock);pending.swap(outbound);}
            bool ok=true;
            for(const auto&m:pending){auto bytes=encode(m);ULONG written=0;if(!WTSVirtualChannelWrite(channel,reinterpret_cast<char*>(bytes.data()),ULONG(bytes.size()),&written)||written!=bytes.size()){ok=false;break;}}
            if(!ok)break;
            char buffer[CHANNEL_PDU_LENGTH]{};ULONG count=0;
            if(!WTSVirtualChannelRead(channel,40,buffer,sizeof(buffer),&count)){if(GetLastError()==ERROR_TIMEOUT)continue;break;}
            if(!count)continue;
            if(count<sizeof(CHANNEL_PDU_HEADER))break;
            try {
                // DVC reads include a CHANNEL_PDU_HEADER for every fragment.
                auto ptr=reinterpret_cast<const uint8_t*>(buffer)+sizeof(CHANNEL_PDU_HEADER);
                auto messages=decoder.feed(ptr,count-sizeof(CHANNEL_PDU_HEADER));
                std::lock_guard<std::mutex>lock(queueLock);
                for(auto&m:messages){
                    if(!requestType(m.type)||m.data.size()>65552)throw std::runtime_error("request limit");
                    auto encoded=encode(m);auto packet=WayWinPolicy::packet(std::string(encoded.begin(),encoded.end()),m);
                    if(inbound.push(std::move(packet))!=WayWinPolicy::Admission::Accepted)throw std::runtime_error("queue limit");
                    if(inbound.replacedRequest&&(negotiated&InputCompletion)){if(outbound.size()>=64)throw std::runtime_error("completion limit");outbound.push_back({InputAck,inbound.replacedRequest,m.epoch,{}});}
                }
            }catch(...){break;}
        }
        linked=false;WTSVirtualChannelClose(channel);
        {std::lock_guard<std::mutex>lock(queueLock);inbound.clear();outbound.clear();}
        Sleep(250);
    }
}
static void install(bool enable) {
    PWSTR local=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)))return;
    fs::path dir=fs::path(local)/L"WayWin",dest=dir/L"WayWin Companion.exe";CoTaskMemFree(local);
    HKEY key=nullptr;auto rc=RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if(rc!=ERROR_SUCCESS){MessageBoxW(mainWindow,L"Could not open per-user startup settings.",L"WayWin",MB_ICONERROR);return;}
    bool ok=true;
    if(enable) {
        wchar_t source[32768]{};GetModuleFileNameW(nullptr,source,32768);std::error_code error;fs::create_directories(dir,error);
        if(error)ok=false;
        if(ok&&fs::path(source)!=dest)ok=CopyFileW(source,dest.c_str(),FALSE)!=0;
        std::wstring command=L"\""+dest.wstring()+L"\" --background";
        if(ok)ok=RegSetValueExW(key,L"WayWin Companion",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),DWORD((command.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;
    } else {auto status=RegDeleteValueW(key,L"WayWin Companion");ok=status==ERROR_SUCCESS||status==ERROR_FILE_NOT_FOUND;}
    RegCloseKey(key);
    MessageBoxW(mainWindow,ok?(enable?L"Installed for this user. Starts at next sign-in. You can close this window to keep it running.":L"Automatic startup disabled. Close WayWin and delete its LocalAppData/WayWin folder to remove the installed copy."):L"Could not update installation. Check file permissions.",L"WayWin",ok?MB_OK:MB_ICONERROR);
}
static LRESULT CALLBACK procedure(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE:{
        statusLabel=CreateWindowW(L"STATIC",L"Starting...",WS_CHILD|WS_VISIBLE,20,20,610,55,w,nullptr,nullptr,nullptr);
        const wchar_t*labels[]={L"Install + start at sign-in",L"Disable startup",L"Windows RDP settings",L"Exit companion"};
        for(int i=0;i<4;++i)CreateWindowW(L"BUTTON",labels[i],WS_CHILD|WS_VISIBLE|WS_TABSTOP,20+(i%2)*310,90+(i/2)*48,290,36,w,reinterpret_cast<HMENU>(INT_PTR(100+i)),nullptr,nullptr);
        auto info=L"Use a supported Windows Remote Desktop / RemoteApp configuration.\r\nNo system binary changes or policy bypasses are applied.\r\nShell images and clipboard text are transferred only on request.\r\nClose hides this app in the notification area.";
        CreateWindowW(L"STATIC",info,WS_CHILD|WS_VISIBLE,20,200,610,100,w,nullptr,nullptr,nullptr);
        NOTIFYICONDATAW tray{};tray.cbSize=sizeof(tray);tray.hWnd=w;tray.uID=1;tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;tray.uCallbackMessage=WM_APP+1;tray.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101));wcscpy_s(tray.szTip,L"WayWin Companion");Shell_NotifyIconW(NIM_ADD,&tray);
        SetTimer(w,1,50,nullptr);return 0;}
    case WM_TIMER:tick();return 0;
    case WM_COMMAND:switch(LOWORD(wp)){case 100:install(true);break;case 101:install(false);break;case 102:ShellExecuteW(w,L"open",L"ms-settings:remotedesktop",nullptr,nullptr,SW_SHOWNORMAL);break;case 103:DestroyWindow(w);break;}return 0;
    case WM_APP+1:if(lp==WM_LBUTTONUP||lp==WM_RBUTTONUP){ShowWindow(w,SW_SHOW);SetForegroundWindow(w);}return 0;
    case WM_CLOSE:ShowWindow(w,SW_HIDE);return 0;
    case WM_DESTROY:{running=false;captureProcess.stop();releaseKeys();NOTIFYICONDATAW tray{};tray.cbSize=sizeof(tray);tray.hWnd=w;tray.uID=1;Shell_NotifyIconW(NIM_DELETE,&tray);PostQuitMessage(0);return 0;}
    default:return DefWindowProcW(w,msg,wp,lp);
    }
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR command,int show) {
    if(wcscmp(command,L"--capture-worker")==0)return runCaptureWorker();
    ProcessIdToSessionId(GetCurrentProcessId(),&sessionId);
    std::wstring mutexName=L"Local\\WayWin.Companion."+std::to_wstring(sessionId);
    HANDLE single=CreateMutexW(nullptr,TRUE,mutexName.c_str());if(!single||GetLastError()==ERROR_ALREADY_EXISTS){if(single)CloseHandle(single);return 0;}
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 1;
    GUID guid{};CoCreateGuid(&guid);wchar_t text[64]{};StringFromGUID2(guid,text,64);identity=utf8(text);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.lpfnWndProc=procedure;cls.hInstance=instance;cls.lpszClassName=L"WayWinCompanion";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101));cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassExW(&cls);
    mainWindow=CreateWindowExW(0,cls.lpszClassName,L"WayWin · Windows Companion",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,670,365,nullptr,nullptr,instance,nullptr);
    if(!mainWindow)return 1;
    if(!wcsstr(command,L"--background"))ShowWindow(mainWindow,show);
    std::thread channel(channelLoop);MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    running=false;channel.join();CoUninitialize();CloseHandle(single);return 0;
}
