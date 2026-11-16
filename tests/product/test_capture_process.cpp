// SPDX-License-Identifier: Apache-2.0
// Windows process/IPC fault injection. This uses the production process owner,
// with a deterministic worker instead of invoking a real Windows shell.
#include "capture_worker.h"
#include <cstdio>
#include <cstdlib>
using namespace WayWinWire;
#define CHECK(value) do {if(!(value)){std::fprintf(stderr,"check failed at line %d\n",__LINE__);return 1;}} while(false)
static int worker() {
    Decoder decoder;uint8_t buffer[256];DWORD count=0;
    while(ReadFile(GetStdHandle(STD_INPUT_HANDLE),buffer,sizeof(buffer),&count,nullptr)&&count){
        for(const auto&m:decoder.feed(buffer,count)){
            wchar_t mode[32]{};GetEnvironmentVariableW(L"WAYWIN_CAPTURE_TEST",mode,32);
            if(wcscmp(mode,L"hang")==0)Sleep(INFINITE);
            if(wcscmp(mode,L"crash")==0)TerminateProcess(GetCurrentProcess(),9);
            if(wcscmp(mode,L"ipc")==0){uint8_t bad[24]{};DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),bad,sizeof(bad),&written,nullptr);continue;}
            Message response{wcscmp(mode,L"failure")==0?SurfaceGone:Frame,m.request,m.epoch,{}};
            wchar_t inherited[32]{};GetEnvironmentVariableW(L"WAYWIN_TEST_HANDLE",inherited,32);
            DWORD flags=0;HANDLE extra=reinterpret_cast<HANDLE>(uintptr_t(_wcstoui64(inherited,nullptr,10)));
            put(response.data,GetHandleInformation(extra,&flags)?0:1);
            void *large=VirtualAlloc(nullptr,300ull*1024*1024,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
            put(response.data,large?0:1);if(large)VirtualFree(large,0,MEM_RELEASE);
            auto bytes=encode(response);DWORD written=0;
            if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),bytes.data(),DWORD(bytes.size()),&written,nullptr)||written!=bytes.size())return 2;
        }
    }
    return 0;
}
static int poll(CaptureProcess &process,uint64_t now,Message &message) {
    for(int i=0;i<200;++i){auto result=process.poll(now,message);if(result)return result;Sleep(10);}return 0;
}
int wmain(int argc,wchar_t **argv) {
    if(argc>1&&wcscmp(argv[1],L"--capture-worker")==0)return worker();
    if(argc>1&&wcscmp(argv[1],L"--parent-death")==0){
        SetEnvironmentVariableW(L"WAYWIN_CAPTURE_TEST",L"hang");CaptureProcess process;
        if(!process.submit(1,1,1,nullptr,0))return 2;
        auto pid=process.processId();DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),&pid,sizeof(pid),&written,nullptr);
        ExitProcess(0); // Deliberately bypass destructors: kernel job closure must collect the child.
    }
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE extra=CreateEventW(&security,TRUE,FALSE,nullptr);
    auto value=std::to_wstring(uintptr_t(extra));SetEnvironmentVariableW(L"WAYWIN_TEST_HANDLE",value.c_str());
    CaptureProcess process;Message response;
    SetEnvironmentVariableW(L"WAYWIN_CAPTURE_TEST",L"success");CHECK(process.submit(1,7,1,nullptr,100));CHECK(poll(process,101,response)==1);
    const auto pid=process.processId();CHECK(response.type==Frame&&response.request==1&&response.epoch==7);Reader read(response.data);CHECK(read.number()==1);CHECK(read.number()==1);
    CHECK(process.submit(2,7,1,nullptr,200));CHECK(poll(process,201,response)==1);CHECK(process.processId()==pid);process.stop();
    SetEnvironmentVariableW(L"WAYWIN_CAPTURE_TEST",L"failure");CHECK(process.submit(3,7,1,nullptr,300));CHECK(poll(process,301,response)==1);CHECK(response.type==SurfaceGone);CHECK(process.available());process.stop();
    for(const wchar_t*mode:{L"hang",L"crash",L"ipc"}){
        SetEnvironmentVariableW(L"WAYWIN_CAPTURE_TEST",mode);CHECK(process.submit(4,7,1,nullptr,1000));const auto child=OpenProcess(SYNCHRONIZE,FALSE,process.processId());CHECK(child);
        CHECK(poll(process,wcscmp(mode,L"hang")==0?6000:1001,response)==-1);CHECK(WaitForSingleObject(child,2000)==WAIT_OBJECT_0);CloseHandle(child);
        CHECK(!process.available());CHECK(!process.submit(5,7,1,nullptr,9000));process.reopen();CHECK(process.available());
    }
    CloseHandle(extra);
    HANDLE input=nullptr,output=nullptr;CHECK(CreatePipe(&input,&output,&security,4096));SetHandleInformation(input,HANDLE_FLAG_INHERIT,0);
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);std::wstring command=L"\""+std::wstring(executable)+L"\" --parent-death";
    STARTUPINFOW start{};start.cb=sizeof(start);start.dwFlags=STARTF_USESTDHANDLES;start.hStdOutput=start.hStdError=output;start.hStdInput=GetStdHandle(STD_INPUT_HANDLE);PROCESS_INFORMATION parent{};
    CHECK(CreateProcessW(executable,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&parent));CloseHandle(output);CloseHandle(parent.hThread);
    DWORD childPid=0,got=0;bool received=false;
    for(int i=0;i<300;++i){DWORD available=0;if(!PeekNamedPipe(input,nullptr,0,nullptr,&available,nullptr))break;if(available>=sizeof(childPid)){received=ReadFile(input,&childPid,sizeof(childPid),&got,nullptr)&&got==sizeof(childPid);break;}Sleep(10);}
    CloseHandle(input);if(!received){TerminateProcess(parent.hProcess,2);WaitForSingleObject(parent.hProcess,2000);CloseHandle(parent.hProcess);CHECK(received);}
    HANDLE child=OpenProcess(SYNCHRONIZE,FALSE,childPid);CHECK(WaitForSingleObject(parent.hProcess,3000)==WAIT_OBJECT_0);CloseHandle(parent.hProcess);
    if(child){CHECK(WaitForSingleObject(child,2000)==WAIT_OBJECT_0);CloseHandle(child);}
    std::puts("capture process contracts passed");return 0;
}
