#include "shell_support.h"
#include <shellapi.h>
#include <gdiplus.h>
#include <objidl.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

using namespace Gdiplus;
namespace lightrec {
namespace {
constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t runValue[] = L"GlobalRecorderNative";
void mark(Graphics& g, bool recording) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0,0,0,0));
    GraphicsPath panel;
    panel.AddArc(1,1,16,16,180,90); panel.AddArc(15,1,16,16,270,90);
    panel.AddArc(15,15,16,16,0,90); panel.AddArc(1,15,16,16,90,90); panel.CloseFigure();
    SolidBrush base(Color(255,27,33,41)); g.FillPath(&base,&panel);
    Pen edge(Color(255,101,128,140),.8f); g.DrawPath(&edge,&panel);
    Pen wave(recording?Color(255,249,119,123):Color(255,178,226,221),3.2f);
    wave.SetStartCap(LineCapRound); wave.SetEndCap(LineCapRound);
    g.DrawLine(&wave,9.f,13.f,9.f,19.f);
    g.DrawLine(&wave,16.f,8.f,16.f,24.f);
    g.DrawLine(&wave,23.f,11.f,23.f,21.f);
    if(recording) {
        SolidBrush rim(Color(255,27,33,41)), lamp(Color(255,255,128,133));
        g.FillEllipse(&rim,23,0,9,9); g.FillEllipse(&lamp,24,1,7,7);
    }
}
bool pngClsid(CLSID& clsid) {
    UINT count=0,bytes=0; GetImageEncodersSize(&count,&bytes);
    if(!bytes) return false;
    std::vector<BYTE> buffer(bytes); auto enc=(ImageCodecInfo*)buffer.data();
    if(GetImageEncoders(count,bytes,enc)!=Ok) return false;
    for(UINT i=0;i<count;++i) if(wcscmp(enc[i].MimeType,L"image/png")==0) {
        clsid=enc[i].Clsid; return true;
    }
    return false;
}
std::vector<BYTE> pngBytes(int pixels,bool recording,const CLSID& encoder) {
    Bitmap bitmap(pixels,pixels,PixelFormat32bppPARGB);
    { Graphics g(&bitmap); g.ScaleTransform(pixels/32.f,pixels/32.f); mark(g,recording); }
    IStream* stream=nullptr;
    if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream))) return {};
    std::vector<BYTE> bytes;
    if(bitmap.Save(stream,&encoder,nullptr)==Ok) {
        STATSTG stat{};
        if(SUCCEEDED(stream->Stat(&stat,STATFLAG_NONAME))&&stat.cbSize.QuadPart<1024*1024) {
            bytes.resize((size_t)stat.cbSize.QuadPart);
            LARGE_INTEGER zero{}; stream->Seek(zero,STREAM_SEEK_SET,nullptr);
            ULONG copied=0;
            if(FAILED(stream->Read(bytes.data(),(ULONG)bytes.size(),&copied))||copied!=bytes.size()) bytes.clear();
        }
    }
    stream->Release(); return bytes;
}
void word(std::ofstream& out,uint16_t value) {
    char bytes[2]={(char)value,(char)(value>>8)}; out.write(bytes,2);
}
void dword(std::ofstream& out,uint32_t value) {
    char bytes[4]={(char)value,(char)(value>>8),(char)(value>>16),(char)(value>>24)}; out.write(bytes,4);
}
}
HICON makeRecorderIcon(int pixels,bool recording) {
    if(pixels<8||pixels>256) return nullptr;
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=pixels; info.bmiHeader.biHeight=-pixels;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;
    HBITMAP color=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!color) return nullptr;
    {
        Bitmap bitmap(pixels,pixels,pixels*4,PixelFormat32bppPARGB,(BYTE*)bits);
        Graphics g(&bitmap); g.ScaleTransform(pixels/32.f,pixels/32.f); mark(g,recording); g.Flush();
    }
    HBITMAP mask=CreateBitmap(pixels,pixels,1,1,nullptr);
    if(!mask) { DeleteObject(color); return nullptr; }
    HDC dc=CreateCompatibleDC(nullptr); auto previous=SelectObject(dc,mask);
    PatBlt(dc,0,0,pixels,pixels,BLACKNESS); SelectObject(dc,previous); DeleteDC(dc);
    ICONINFO icon{}; icon.fIcon=TRUE; icon.hbmColor=color; icon.hbmMask=mask;
    HICON result=CreateIconIndirect(&icon);
    DeleteObject(mask); DeleteObject(color); return result;
}
bool exportRecorderIcons(const std::filesystem::path& folder) {
    std::error_code ec; std::filesystem::create_directories(folder,ec);
    if(ec) return false;
    CLSID encoder; if(!pngClsid(encoder)) return false;
    const int sizes[]={16,20,24,32,48,64,128,256};
    for(bool recording:{false,true}) {
        std::vector<std::vector<BYTE>> images;
        for(int size:sizes) {
            auto image=pngBytes(size,recording,encoder); if(image.empty()) return false;
            images.push_back(std::move(image));
        }
        std::ofstream out(folder/(recording?L"recorder-recording.ico":L"recorder.ico"),std::ios::binary);
        word(out,0); word(out,1); word(out,(uint16_t)images.size());
        uint32_t offset=6+(uint32_t)images.size()*16;
        for(size_t i=0;i<images.size();++i) {
            out.put((char)(sizes[i]==256?0:sizes[i])); out.put((char)(sizes[i]==256?0:sizes[i]));
            word(out,0); word(out,1); word(out,32); dword(out,(uint32_t)images[i].size()); dword(out,offset);
            offset+=(uint32_t)images[i].size();
        }
        for(const auto& image:images) out.write((const char*)image.data(),image.size());
        if(!out) return false;
        auto png=pngBytes(128,recording,encoder);
        std::ofstream thumbnail(folder/(recording?L"recorder-recording.png":L"recorder.png"),std::ios::binary);
        thumbnail.write((const char*)png.data(),png.size()); if(!thumbnail) return false;
    }
    // Preview both states on a light and a dark taskbar background.
    Bitmap sheet(720,280,PixelFormat32bppPARGB);
    Graphics g(&sheet); g.Clear(Color(255,241,244,246));
    SolidBrush dark(Color(255,28,34,43)); g.FillRectangle(&dark,360,0,360,280);
    for(int side=0;side<2;++side) for(int state=0;state<2;++state) {
        int x=side*360+65+state*150;
        auto image=pngBytes(88,state!=0,encoder);
        IStream* stream=nullptr; CreateStreamOnHGlobal(nullptr,TRUE,&stream);
        if(!stream) return false;
        ULONG copied=0; stream->Write(image.data(),(ULONG)image.size(),&copied);
        LARGE_INTEGER zero{}; stream->Seek(zero,STREAM_SEEK_SET,nullptr);
        Bitmap* icon=Bitmap::FromStream(stream);
        g.DrawImage(icon,x,53,88,88); delete icon; stream->Release();
        Font font(L"Microsoft YaHei UI",16,FontStyleRegular,UnitPixel);
        SolidBrush label(side?Color(255,209,224,231):Color(255,62,80,92));
        g.DrawString(state?L"录音中":L"待机",-1,&font,PointF((REAL)x+18,165),&label);
        // Native 16 px mark at 2x scale for checking the small icon silhouette.
        auto small=pngBytes(16,state!=0,encoder);
        IStream* smallStream=nullptr; CreateStreamOnHGlobal(nullptr,TRUE,&smallStream);
        if(!smallStream) return false;
        smallStream->Write(small.data(),(ULONG)small.size(),&copied); smallStream->Seek(zero,STREAM_SEEK_SET,nullptr);
        Bitmap* mini=Bitmap::FromStream(smallStream);
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g.DrawImage(mini,x+28,209,32,32); delete mini; smallStream->Release();
    }
    return sheet.Save((folder/L"托盘图标预览.png").c_str(),&encoder,nullptr)==Ok;
}
std::wstring startupCommand(const std::wstring& executable) {
    // Windows paths cannot contain a quote; require an absolute exe path.
    if(executable.empty()||executable.find(L'"')!=std::wstring::npos||
       !std::filesystem::path(executable).is_absolute()) return {};
    return L"\""+executable+L"\" --background";
}
LONG readStartup(std::wstring& command,const wchar_t* subkey) {
    command.clear(); DWORD bytes=0;
    const wchar_t* key=subkey?subkey:runKey;
    LONG result=RegGetValueW(HKEY_CURRENT_USER,key,runValue,RRF_RT_REG_SZ,nullptr,nullptr,&bytes);
    if(result!=ERROR_SUCCESS) return result;
    if(bytes<sizeof(wchar_t)||bytes>32768*sizeof(wchar_t)) return ERROR_INVALID_DATA;
    std::vector<wchar_t> value(bytes/sizeof(wchar_t)+1,0);
    result=RegGetValueW(HKEY_CURRENT_USER,key,runValue,RRF_RT_REG_SZ,nullptr,value.data(),&bytes);
    if(result==ERROR_SUCCESS) command=value.data();
    return result;
}
LONG writeStartup(bool enabled,const std::wstring& executable,const wchar_t* subkey) {
    const wchar_t* path=subkey?subkey:runKey;
    std::wstring command=startupCommand(executable);
    if(enabled&&(command.empty()||command.size()>=260)) return ERROR_INVALID_PARAMETER;
    HKEY key=nullptr; LONG result;
    if(enabled) result=RegCreateKeyExW(HKEY_CURRENT_USER,path,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    else result=RegOpenKeyExW(HKEY_CURRENT_USER,path,0,KEY_SET_VALUE,&key);
    if(!enabled&&result==ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if(result!=ERROR_SUCCESS) return result;
    if(enabled) result=RegSetValueExW(key,runValue,0,REG_SZ,(const BYTE*)command.c_str(),
                                    (DWORD)((command.size()+1)*sizeof(wchar_t)));
    else {
        result=RegDeleteValueW(key,runValue);
        if(result==ERROR_FILE_NOT_FOUND) result=ERROR_SUCCESS;
    }
    RegCloseKey(key); return result;
}
bool startupSelfTest(std::wstring& issue) {
    std::wstring key=L"Software\\GlobalRecorderNative-Test-"+std::to_wstring(GetCurrentProcessId())+
        L"-"+std::to_wstring(GetTickCount64());
    std::wstring exe=L"D:\\录音工具 测试\\GlobalRecorder.exe",stored;
    bool ok=writeStartup(false,exe,key.c_str())==ERROR_SUCCESS &&
        writeStartup(true,exe,key.c_str())==ERROR_SUCCESS &&
        readStartup(stored,key.c_str())==ERROR_SUCCESS && stored==startupCommand(exe);
    int count=0; LPWSTR* args=CommandLineToArgvW(stored.c_str(),&count);
    ok=ok&&args&&count==2&&args[0]==exe&&std::wstring(args[1])==L"--background";
    if(args) LocalFree(args);
    HKEY handle=nullptr;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,key.c_str(),0,KEY_SET_VALUE,&handle)==ERROR_SUCCESS) {
        DWORD sentinel=7;
        RegSetValueExW(handle,L"Unrelated",0,REG_DWORD,(BYTE*)&sentinel,sizeof(sentinel)); RegCloseKey(handle);
    } else ok=false;
    ok=ok&&writeStartup(false,exe,key.c_str())==ERROR_SUCCESS &&
       readStartup(stored,key.c_str())==ERROR_FILE_NOT_FOUND;
    DWORD sentinel=0,bytes=sizeof(sentinel);
    ok=ok&&RegGetValueW(HKEY_CURRENT_USER,key.c_str(),L"Unrelated",RRF_RT_REG_DWORD,nullptr,
                       &sentinel,&bytes)==ERROR_SUCCESS&&sentinel==7;
    ok=ok&&writeStartup(false,exe,key.c_str())==ERROR_SUCCESS &&
        writeStartup(true,L"relative.exe",key.c_str())==ERROR_INVALID_PARAMETER;
    LONG removed=RegDeleteKeyW(HKEY_CURRENT_USER,key.c_str());
    ok=ok&&(removed==ERROR_SUCCESS||removed==ERROR_FILE_NOT_FOUND);
    if(!ok) issue=L"隔离启动项测试失败（没有修改 Windows Run 启动项）";
    return ok;
}
bool traySelfTest(HINSTANCE instance,std::wstring& issue) {
    WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=instance;
    wc.lpszClassName=L"LightRecTrayDiagnostic"; RegisterClassW(&wc);
    HWND hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"",WS_POPUP,0,0,1,1,
                              nullptr,nullptr,instance,nullptr);
    HICON icon=makeRecorderIcon(32,false);
    bool ok=hwnd&&icon;
    NOTIFYICONDATAW data{}; data.cbSize=sizeof(data); data.hWnd=hwnd;
    data.uID=0x7A11; data.uFlags=NIF_ICON|NIF_TIP; data.hIcon=icon;
    wcscpy_s(data.szTip,L"全局录音 · 图标自检");
    bool added=false;
    if(ok) { added=Shell_NotifyIconW(NIM_ADD,&data)!=FALSE; ok=added; }
    if(added) {
        data.uVersion=NOTIFYICON_VERSION_4; ok=Shell_NotifyIconW(NIM_SETVERSION,&data)&&ok;
        NOTIFYICONIDENTIFIER id{}; id.cbSize=sizeof(id); id.hWnd=hwnd; id.uID=data.uID;
        RECT rect{}; ok=SUCCEEDED(Shell_NotifyIconGetRect(&id,&rect))&&ok;
        Shell_NotifyIconW(NIM_DELETE,&data);
        // Verify our own item can be added again, without restarting Explorer.
        bool recreated=Shell_NotifyIconW(NIM_ADD,&data)!=FALSE; ok=ok&&recreated;
        if(recreated) Shell_NotifyIconW(NIM_DELETE,&data);
    }
    if(hwnd) DestroyWindow(hwnd); if(icon) DestroyIcon(icon);
    if(!ok) issue=L"托盘 API 自检失败或资源管理器通知区域不可用";
    return ok;
}
}
