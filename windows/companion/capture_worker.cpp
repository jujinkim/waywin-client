// SPDX-License-Identifier: Apache-2.0
#include "capture_worker.h"
#include <wincodec.h>
#include <dwmapi.h>
#include <filesystem>
namespace fs=std::filesystem;
using namespace WayWinWire;
bool shellAllowed(HWND w,uint32_t kind) {
    DWORD currentSession=0;ProcessIdToSessionId(GetCurrentProcessId(),&currentSession);
    if(!IsWindowVisible(w)||IsIconic(w))return false;
    DWORD pid=0,sid=0;GetWindowThreadProcessId(w,&pid);if(!ProcessIdToSessionId(pid,&sid)||sid!=currentSession)return false;
    BOOL cloaked=FALSE;DwmGetWindowAttribute(w,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));if(cloaked)return false;
    HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!p)return false;
    wchar_t path[32768]{};DWORD n=32768;bool ok=QueryFullProcessImageNameW(p,0,path,&n)!=0;CloseHandle(p);if(!ok)return false;
    std::wstring name=fs::path(path).filename().wstring();std::transform(name.begin(),name.end(),name.begin(),towlower);
    if(kind==1)return name==L"startmenuexperiencehost.exe";
    if(kind==2) {
        if(name==L"systemsettings.exe")return true;
        if(name==L"applicationframehost.exe") {
            bool settings=false;
            EnumChildWindows(w,[](HWND child,LPARAM arg)->BOOL{
                DWORD owner=0;GetWindowThreadProcessId(child,&owner);HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,owner);
                if(process){wchar_t path[32768]{};DWORD count=32768;if(QueryFullProcessImageNameW(process,0,path,&count)&&_wcsicmp(fs::path(path).filename().c_str(),L"SystemSettings.exe")==0)*reinterpret_cast<bool*>(arg)=true;CloseHandle(process);}return TRUE;
            },reinterpret_cast<LPARAM>(&settings));
            return settings;
        }
        return false;
    }
    if(kind==4)return name==L"textinputhost.exe"||name==L"inputapp.exe";
    if(kind==3) {
        if(name==L"shellexperiencehost.exe")return true;
        wchar_t cls[256]{};GetClassNameW(w,cls,256);
        return name==L"explorer.exe"&&(wcscmp(cls,L"ControlCenterWindow")==0||wcscmp(cls,L"XamlExplorerHostIslandWindow")==0);
    }
    return false;
}
static std::vector<uint8_t> capture(HWND w,IWICImagingFactory*imaging) {
    RECT rc{};if(!GetWindowRect(w,&rc))return {};
    const int width=rc.right-rc.left,height=rc.bottom-rc.top;
    if(width<=0||height<=0||width>8192||height>8192||uint64_t(width)*uint64_t(height)>WayWinPolicy::CapturePixels)return {};
    HDC screen=GetDC(nullptr),mem=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);HGDIOBJ old=SelectObject(mem,bitmap);
    // PrintWindow captures only this approved shell HWND; never copy the whole desktop.
    BOOL ok=PrintWindow(w,mem,2);SelectObject(mem,old);DeleteDC(mem);ReleaseDC(nullptr,screen);
    if(!ok){DeleteObject(bitmap);return {};}
    IWICBitmap* source=nullptr;IWICBitmapScaler* scaler=nullptr;IWICFormatConverter* convert=nullptr;
    IStream* stream=nullptr;IWICBitmapEncoder* encoder=nullptr;IWICBitmapFrameEncode* frame=nullptr;
    std::vector<uint8_t> bytes;
    const UINT outW=UINT(std::min(width,1600)),outH=UINT((int64_t(height)*outW)/width);
    HRESULT hr=imaging->CreateBitmapFromHBITMAP(bitmap,nullptr,WICBitmapIgnoreAlpha,&source);
    if(SUCCEEDED(hr))hr=imaging->CreateBitmapScaler(&scaler);
    if(SUCCEEDED(hr))hr=scaler->Initialize(source,outW,outH,WICBitmapInterpolationModeFant);
    if(SUCCEEDED(hr))hr=imaging->CreateFormatConverter(&convert);
    if(SUCCEEDED(hr))hr=convert->Initialize(scaler,GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
    if(SUCCEEDED(hr))hr=CreateStreamOnHGlobal(nullptr,TRUE,&stream);
    if(SUCCEEDED(hr))hr=imaging->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder);
    if(SUCCEEDED(hr))hr=encoder->Initialize(stream,WICBitmapEncoderNoCache);
    if(SUCCEEDED(hr))hr=encoder->CreateNewFrame(&frame,nullptr);
    if(SUCCEEDED(hr))hr=frame->Initialize(nullptr);
    if(SUCCEEDED(hr))hr=frame->SetSize(outW,outH);
    WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;
    if(SUCCEEDED(hr))hr=frame->SetPixelFormat(&format);
    if(SUCCEEDED(hr))hr=frame->WriteSource(convert,nullptr);
    if(SUCCEEDED(hr))hr=frame->Commit();
    if(SUCCEEDED(hr))hr=encoder->Commit();
    if(SUCCEEDED(hr)) {
        STATSTG stat{};hr=stream->Stat(&stat,STATFLAG_NONAME);
        if(SUCCEEDED(hr)&&stat.cbSize.QuadPart<Maximum-16) {
            LARGE_INTEGER zero{};stream->Seek(zero,STREAM_SEEK_SET,nullptr);
            bytes.resize(size_t(stat.cbSize.QuadPart));ULONG got=0;
            if(FAILED(stream->Read(bytes.data(),ULONG(bytes.size()),&got))||got!=bytes.size())bytes.clear();
        }
    }
    if(frame)frame->Release();
    if(encoder)encoder->Release();
    if(stream)stream->Release();
    if(convert)convert->Release();
    if(scaler)scaler->Release();
    if(source)source->Release();
    DeleteObject(bitmap);return bytes;
}

int runCaptureWorker() {
    HANDLE input=GetStdHandle(STD_INPUT_HANDLE),output=GetStdHandle(STD_OUTPUT_HANDLE);
    if(!input||!output||GetFileType(input)!=FILE_TYPE_PIPE||GetFileType(output)!=FILE_TYPE_PIPE)return 2;
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 2;
    IWICImagingFactory *factory=nullptr;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))){CoUninitialize();return 2;}
    int status=0;Decoder decoder;
    try {
        uint8_t buffer[256];DWORD count=0;
        while(ReadFile(input,buffer,sizeof(buffer),&count,nullptr)&&count) {
            for(const auto&m:decoder.feed(buffer,count)) {
                Reader read(m.data);auto kind=read.number();auto low=read.number(),high=read.number();
                if(!read.done()||m.type!=Shell||kind<1||kind>4)throw std::runtime_error("capture IPC");
                HWND window=reinterpret_cast<HWND>(uintptr_t(uint64_t(low)|(uint64_t(high)<<32)));
                RECT rect{};std::vector<uint8_t> bytes;
                if(shellAllowed(window,kind)&&GetWindowRect(window,&rect))bytes=capture(window,factory);
                Message response{bytes.empty()?SurfaceGone:Frame,m.request,m.epoch,{}};
                if(!bytes.empty()){put(response.data,kind);put(response.data,uint32_t(rect.right-rect.left));put(response.data,uint32_t(rect.bottom-rect.top));response.data.insert(response.data.end(),bytes.begin(),bytes.end());}
                auto encoded=encode(response);size_t offset=0;
                while(offset<encoded.size()){DWORD written=0;if(!WriteFile(output,encoded.data()+offset,DWORD(encoded.size()-offset),&written,nullptr)||!written)throw std::runtime_error("capture IPC");offset+=written;}
            }
        }
    }catch(...){status=2;}
    factory->Release();CoUninitialize();return status;
}
