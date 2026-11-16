// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include "waywin_policy.hpp"
#include <string>
bool shellAllowed(HWND window,uint32_t kind);
int runCaptureWorker();
// Only two pipe handles are inherited. The job is assigned atomically at
// creation so parent death cannot strand a suspended child before assignment.
class CaptureProcess {
    HANDLE job=nullptr,process=nullptr,input=nullptr,output=nullptr;
    WayWinWire::Decoder decoder;
    WayWinWire::Message pending;
    WayWinPolicy::CaptureDeadline deadline;
    bool launch() {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
        HANDLE childInput=nullptr,childOutput=nullptr;
        if(!CreatePipe(&childInput,&input,&security,4096))return false;
        if(!CreatePipe(&output,&childOutput,&security,65536)){CloseHandle(childInput);stop();return false;}
        SetHandleInformation(input,HANDLE_FLAG_INHERIT,0);SetHandleInformation(output,HANDLE_FLAG_INHERIT,0);
        job=CreateJobObjectW(nullptr,nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_JOB_MEMORY|JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.ProcessMemoryLimit=WayWinPolicy::CaptureMemory;limits.JobMemoryLimit=WayWinPolicy::CaptureMemory;limits.BasicLimitInformation.ActiveProcessLimit=1;
        bool ok=job&&SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits));
        SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,2,0,&size);
        std::vector<uint8_t> storage(size);
        auto attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        const bool initialized=InitializeProcThreadAttributeList(attributes,2,0,&size)!=FALSE;
        HANDLE inherited[]={childInput,childOutput};
        ok=ok&&initialized&&UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)
            &&UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,&job,sizeof(job),nullptr,nullptr);
        STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof(start);start.lpAttributeList=attributes;
        start.StartupInfo.dwFlags=STARTF_USESTDHANDLES;start.StartupInfo.hStdInput=childInput;start.StartupInfo.hStdOutput=childOutput;start.StartupInfo.hStdError=childOutput;
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
        std::wstring command=L"\""+std::wstring(executable)+L"\" --capture-worker";PROCESS_INFORMATION info{};
        if(ok)ok=CreateProcessW(executable,command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW,nullptr,nullptr,&start.StartupInfo,&info)!=FALSE;
        if(initialized)DeleteProcThreadAttributeList(attributes);
        CloseHandle(childInput);CloseHandle(childOutput);
        if(!ok){stop();return false;}
        process=info.hProcess;CloseHandle(info.hThread);return true;
    }
public:
    ~CaptureProcess(){stop();}
    void stop(){
        if(job){TerminateJobObject(job,1);CloseHandle(job);job=nullptr;}
        if(process){WaitForSingleObject(process,1000);CloseHandle(process);process=nullptr;}
        if(input){CloseHandle(input);input=nullptr;}if(output){CloseHandle(output);output=nullptr;}
        decoder.reset();deadline.complete();
    }
    void fault(){stop();deadline.fault();}
    void reopen(){deadline.reopen();}
    bool available()const{return deadline.available();}
    DWORD processId()const{return process?GetProcessId(process):0;}
    bool submit(uint32_t id,uint32_t epoch,uint32_t kind,HWND window,uint64_t now){
        if(!deadline.available())return false;
        if(!process&&!launch()){fault();return false;}
        pending={WayWinWire::Shell,id,epoch,{}};WayWinWire::put(pending.data,kind);
        auto handle=uint64_t(reinterpret_cast<uintptr_t>(window));WayWinWire::put(pending.data,uint32_t(handle));WayWinWire::put(pending.data,uint32_t(handle>>32));
        auto bytes=WayWinWire::encode(pending);DWORD written=0;
        if(!WriteFile(input,bytes.data(),DWORD(bytes.size()),&written,nullptr)||written!=bytes.size()){fault();return false;}
        return deadline.start(now);
    }
    // 0 = pending/idle; 1 = result; -1 = timeout, exit, or invalid IPC.
    int poll(uint64_t now,WayWinWire::Message &response){
        if(!process)return 0;
        if(WaitForSingleObject(process,0)!=WAIT_TIMEOUT||deadline.expired(now)){fault();return -1;}
        DWORD available=0;
        if(!PeekNamedPipe(output,nullptr,0,nullptr,&available,nullptr)){fault();return -1;}
        if(!available)return 0;
        uint8_t buffer[32768];DWORD read=0;
        if(!ReadFile(output,buffer,std::min<DWORD>(available,DWORD(sizeof(buffer))),&read,nullptr)||!read){fault();return -1;}
        try{auto messages=decoder.feed(buffer,read);if(messages.empty())return 0;
            if(messages.size()!=1||!deadline.busy()||messages[0].request!=pending.request||messages[0].epoch!=pending.epoch||(messages[0].type!=WayWinWire::Frame&&messages[0].type!=WayWinWire::SurfaceGone))throw std::runtime_error("capture IPC");
            response=std::move(messages[0]);deadline.complete();return 1;
        }catch(...){fault();return -1;}
    }
};
