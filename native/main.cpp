#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <gdiplus.h>
#include <psapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <cwctype>
#include "audio.h"
#include "shell_support.h"

using namespace Gdiplus;
namespace fs = std::filesystem;
namespace {
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr int W = 448, H = 126;
constexpr ULONGLONG DockHideMs=420, DockShowMs=340;
namespace ui {
const wchar_t* const font=L"Microsoft YaHei UI";
const Color panel(255,27,33,41), border(58,195,213,225);
const Color primary(255,245,248,249), secondary(255,160,179,188);
const Color accent(255,194,225,222), danger(255,244,117,118);
}
enum { Folder = 101, OpenFolder, ShowText, HideWindow, ExitApp,
       CallOff, CallAsk, CallAuto, Retry, ShowWindowCmd, ScopeGlobal, ScopeProgram, EditHotkeys, ToggleStartup,
       ResetCloseAction };
HWND window = nullptr, transcript = nullptr, edit = nullptr;
fs::path project, config, root, session, activeRoot, model, python;
fs::path diagnosticFrame;
lightrec::Recorder recorder;
bool enabled = false, paused = false, expanded = false, diagnostic = false;
bool quitWhenSaved = false, hotkeyR = false, hotkeyT = false;
int callMode = 0, callHits = 0, hover = -1, pressed = -1, benchTicks = 0;
bool callLatched = false, previewRecording = false;
bool programOnly = false;
bool sessionProgramOnly = false;
std::wstring targetExe;
std::wstring sessionTarget;
DWORD preferredPid = 0;
HANDLE asr = nullptr, asrJob = nullptr, instanceMutex = nullptr;
std::vector<fs::path> sessionQueue;
std::wstring note, transcriptCache;
float dpi = 1;
ULONG_PTR gdipToken = 0;
NOTIFYICONDATAW tray{};
std::wstring currentExecutable, lastTrayTip;
HICON appLargeIcon=nullptr, appSmallIcon=nullptr, trayIdleIcon=nullptr, trayRecordingIcon=nullptr;
bool trayAdded=false;
UINT taskbarRestartMessage=0;
ULONGLONG trayLastAttempt=0;
struct Program { DWORD pid, parent; std::wstring name, path; };
std::vector<Program> programs;
HWND picker=nullptr, programList=nullptr, programSearch=nullptr;
HFONT pickerFont=nullptr;
std::vector<size_t> filteredPrograms;
HWND hotkeyDialog=nullptr, recordKeyInput=nullptr, pauseKeyInput=nullptr;
struct Shortcut { UINT modifiers, key; };
Shortcut recordKey{MOD_CONTROL|MOD_ALT,'R'}, pauseKey{MOD_CONTROL|MOD_ALT,'T'};
enum class DockEdge { None, Left, Right, Top, Bottom };
DockEdge dockEdge=DockEdge::None;
RECT dockExpanded{}, dockArea{}, dragOrigin{};
POINT dragStart{}, dragOffset{};
bool dockCollapsed=false, dragPending=false, dragging=false;
bool dockAnimating=false, dockHiding=false, dockHoverArmed=true;
float dockReveal=1, dockAnimationFrom=1;
ULONGLONG dockAnimationStart=0;
ULONGLONG outsideSince=0;
int interactionDepth=0;
enum class CloseAction { Ask, Tray, Exit };
CloseAction closeAction=CloseAction::Ask;
HWND closeDialog=nullptr;
HFONT closeFont=nullptr;
bool closeExitOnly=false, closeIssue=false;
float closeScale=1;
void expandDock(bool animate=false);
void requestExit(bool confirmed=false);
void showCloseChoice(bool exitOnly=false);

std::vector<Program> runningPrograms() {
    std::vector<Program> result;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{}; entry.dwSize=sizeof(entry);
    if(Process32FirstW(snapshot,&entry)) do {
        if(entry.th32ProcessID==GetCurrentProcessId()) continue;
        HANDLE handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);
        if(!handle) continue;
        wchar_t path[32768]; DWORD size=32768;
        if(QueryFullProcessImageNameW(handle,0,path,&size))
            result.push_back({entry.th32ProcessID,entry.th32ParentProcessID,entry.szExeFile,path});
        CloseHandle(handle);
    } while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);
    std::sort(result.begin(),result.end(),[](const Program& a,const Program& b){
        int name=_wcsicmp(a.name.c_str(),b.name.c_str());
        return name ? name<0 : a.pid<b.pid;
    });
    return result;
}
DWORD rootPid(const std::vector<Program>& list,const std::wstring& exe,DWORD preferred) {
    for(const auto& p:list)
        if(p.pid==preferred && _wcsicmp(p.path.c_str(),exe.c_str())==0) return p.pid;
    for(const auto& p:list) {
        if(_wcsicmp(p.path.c_str(),exe.c_str())!=0) continue;
        bool child=false;
        for(const auto& parent:list)
            if(parent.pid==p.parent && _wcsicmp(parent.path.c_str(),exe.c_str())==0) { child=true; break; }
        if(!child) return p.pid;
    }
    return 0;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
    return r;
}
std::string escape(const std::wstring& value) {
    std::string s = utf8(value), out;
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c < 32) {
            const char* hex = "0123456789abcdef";
            out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
        } else out += c;
    }
    return "\"" + out + "\"";
}
bool atomicText(const fs::path& file, const std::string& text) {
    fs::path tmp = file; tmp += L".native-tmp";
    { std::ofstream out(tmp, std::ios::binary); out << text; out.flush(); if (!out) return false; }
    for(int attempt=0;attempt<6;++attempt) {
        if(MoveFileExW(tmp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return true;
        DWORD code=GetLastError();
        if(code!=ERROR_SHARING_VIOLATION&&code!=ERROR_ACCESS_DENIED) break;
        Sleep(10);
    }
    return false;
}
std::wstring ini(const wchar_t* name, const std::wstring& fallback = L"") {
    std::vector<wchar_t> value(32768);
    GetPrivateProfileStringW(L"recorder", name, fallback.c_str(), value.data(),
                             (DWORD)value.size(), config.c_str());
    return value.data();
}
bool setting(const wchar_t* name, const std::wstring& value) {
    HANDLE fresh=CreateFileW(config.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,0,nullptr);
    if(fresh!=INVALID_HANDLE_VALUE) {
        const BYTE bom[]={0xff,0xfe}; DWORD written=0;
        WriteFile(fresh,bom,2,&written,nullptr); CloseHandle(fresh);
    }
    return WritePrivateProfileStringW(L"recorder", name, value.c_str(), config.c_str());
}
bool busy() { return !recorder.finished(); }
bool flags() {
    return atomicText(project / L"native-control.json",
        std::string("{\"enabled\":") + (enabled ? "true" : "false") +
        ",\"paused\":" + (paused ? "true" : "false") +
        ",\"recording\":" + (busy() ? "true" : "false") +
        ",\"session\":" + escape(session.wstring()) + "}");
}
std::wstring quote(const std::wstring& s) {
    // Windows argv quoting, including terminal backslashes.
    std::wstring out = L"\""; size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(slashes * (c == L'"' ? 2 : 1), L'\\'); slashes = 0;
        if (c == L'"') out += L'\\';
        out += c;
    }
    out.append(slashes * 2, L'\\'); return out + L"\"";
}
void locateModel() {
    auto localPath = [](const std::wstring& value, const fs::path& fallback) {
        fs::path path = value.empty() ? fallback : fs::path(value);
        return path.is_absolute() ? path : project / path;
    };
    python = localPath(ini(L"asr_python"), L".venv-asr/Scripts/pythonw.exe");
    if (ini(L"asr_python").empty() && !fs::exists(python))
        python = project / L".venv-asr/Scripts/python.exe";
    model = localPath(ini(L"asr_model"), L"models/faster-whisper-small");
    if (!fs::exists(model / L"model.bin") || !fs::exists(model / L"config.json"))
        model.clear();
}
void pollAsr() {
    if (!asr || WaitForSingleObject(asr, 0) != WAIT_OBJECT_0) return;
    DWORD code = 0; GetExitCodeProcess(asr, &code);
    CloseHandle(asr); asr = nullptr;
    if (code) { paused = true; flags(); note = L"转写出错，录音继续 · 菜单可重试"; }
}
bool pending(const fs::path& target) {
    if (target.empty()) return false;
    std::error_code ec;
    for (const auto& file : fs::directory_iterator(target / L"chunks", ec)) {
        if (file.path().extension() != L".job") continue;
        std::ifstream in(file.path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        auto key=text.find("\"status\"");
        if(key==std::string::npos) continue;
        auto colon=text.find(':',key+8);
        if(colon==std::string::npos) continue;
        auto value=text.find_first_not_of(" \t\r\n",colon+1);
        if(value!=std::string::npos &&
           (text.compare(value,9,"\"pending\"")==0 || text.compare(value,9,"\"running\"")==0)) return true;
    }
    return false;
}
void saveQueue() {
    std::string lines;
    for(const auto& path:sessionQueue) lines+=utf8(path.wstring())+"\n";
    atomicText(project/L"native-session-queue.txt",lines);
}
void startAsr(bool retry = false) {
    pollAsr();
    if (asr || !enabled || paused || diagnostic || session.empty()) return;
    fs::path target;
    if(retry) target=session;
    else {
        bool changed=false;
        for(auto it=sessionQueue.begin();it!=sessionQueue.end();) {
            if(pending(*it)) { target=*it; break; }
            if(*it!=session || !busy()) { it=sessionQueue.erase(it); changed=true; }
            else ++it;
        }
        if(changed) saveQueue();
        if(target.empty()) return;
    }
    if (python.empty() || model.empty() || !fs::exists(python)) {
        note = L"未找到本地模型，录音仍可使用"; paused = true; flags(); return;
    }
    auto control = project / L"native-control.json";
    std::wstring cmd = quote(python.wstring()) + L" -B " +
        quote((project / L"native/native_transcribe.py").wstring()) +
        L" --root " + quote(target.parent_path().wstring()) + L" --session " + quote(target.wstring()) +
        L" --model " + quote(model.wstring()) + L" --control " + quote(control.wstring()) +
        (retry ? L" --retry" : L"");
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(python.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, project.c_str(), &si, &pi)) {
        note = L"无法启动转写进程"; paused = true; flags(); return;
    }
    if (!asrJob) {
        asrJob = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!asrJob || !SetInformationJobObject(asrJob, JobObjectExtendedLimitInformation,
                                               &limits, sizeof(limits))) {
            if (asrJob) CloseHandle(asrJob);
            asrJob = nullptr;
        }
    }
    if (!asrJob || !AssignProcessToJobObject(asrJob, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 3); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        note = L"转写进程隔离失败"; paused = true; flags(); return;
    }
    asr = pi.hProcess; ResumeThread(pi.hThread); CloseHandle(pi.hThread);
}
void chunkReady(const lightrec::Chunk& c) {
    fs::path path(c.path); path.replace_extension(L".job");
    std::ostringstream text;
    text << "{\"audio\":" << escape(c.path) << ",\"source\":" << escape(c.source)
         << ",\"start\":" << std::fixed << std::setprecision(3) << c.start
         << ",\"duration\":" << c.duration << ",\"status\":\"pending\"}";
    if (!atomicText(path, text.str())) throw std::runtime_error("Cannot save transcription job");
}
void recordToggle() {
    if (diagnostic) return;
    if (busy()) { recorder.stop(); note = L"正在保存双轨与混音…"; flags(); return; }
    DWORD targetPid=0;
    if(programOnly) {
        if(!lightrec::processLoopbackSupported()) {
            note=L"当前系统不支持按程序录音，请选择全局声音"; return;
        }
        targetPid=rootPid(runningPrograms(),targetExe,preferredPid);
        if(!targetPid) { note=L"所选程序尚未运行，请先打开程序"; return; }
    }
    pollAsr();
    std::error_code ec; fs::create_directories(root, ec);
    if (ec) { note = L"保存目录不可写，请重新选择文件夹"; return; }
    SYSTEMTIME t; GetLocalTime(&t); wchar_t name[96];
    swprintf(name, 96, L"轻量_%04u%02u%02u_%02u%02u%02u_%lu_%llu",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
        GetCurrentProcessId(), GetTickCount64());
    session = root / name; activeRoot = root;
    sessionProgramOnly=programOnly; sessionTarget=targetExe;
    sessionQueue.push_back(session); saveQueue();
    setting(L"last_session", session.wstring());
    note.clear();
    recorder.start(session.wstring(), chunkReady, targetPid); flags();
}
std::wstring duration() {
    int seconds = previewRecording ? 143 : (int)recorder.elapsed();
    wchar_t text[32]; swprintf(text, 32, L"%02d:%02d:%02d", seconds/3600, seconds/60%60, seconds%60);
    return text;
}
void rounded(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    p.AddArc(x,y,r*2,r*2,180,90); p.AddArc(x+w-r*2,y,r*2,r*2,270,90);
    p.AddArc(x+w-r*2,y+h-r*2,r*2,r*2,0,90); p.AddArc(x,y+h-r*2,r*2,r*2,90,90);
    p.CloseFigure();
}
void text(Graphics& g, const wchar_t* s, float x, float y, float w, float h,
          float size, Color color, const wchar_t* family=ui::font, bool bold=false) {
    Font font(family,size,bold ? FontStyleBold : FontStyleRegular,UnitPixel);
    SolidBrush brush(color); StringFormat format;
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    format.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(s,-1,&font,RectF(x,y,w,h),&format,&brush);
}
int hit(float x, float y) {
    if (y >= 17 && y < 39) {
        if (x >= 398 && x < 424) return 6;
    }
    if (y >= 32 && y <= 88) {
        if (x >= 28 && x <= 84) return 0;
        if (x >= 258 && x <= 322) return 1;
        if (x >= 326 && x <= 366) return 2;
        if (x >= 374 && x <= 418) return 3;
    }
    if (y > 91 && x >= 32 && x <= 416) return 4;
    return -1;
}
void paint(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0,0,0,0));
    GraphicsPath shadow; rounded(shadow,15,22,418,90,29);
    SolidBrush shade(Color(30,0,0,0)); g.FillPath(&shade,&shadow);
    GraphicsPath panel; rounded(panel,12,13,424,94,27);
    SolidBrush fill(Color(224,ui::panel.GetR(),ui::panel.GetG(),ui::panel.GetB())); g.FillPath(&fill,&panel);
    Pen edge(ui::border,1); g.DrawPath(&edge,&panel);
    if(hover==6 || pressed==6) {
        GraphicsPath background; rounded(background,398,17,26,22,7);
        SolidBrush brush(Color(180,187,67,77)); g.FillPath(&brush,&background);
    }
    Pen caption((hover==6||pressed==6)?Color(255,255,241,242):Color(245,182,202,214),1.5f);
    g.DrawLine(&caption,406.f,24.f,414.f,32.f);
    g.DrawLine(&caption,414.f,24.f,406.f,32.f);
    bool running = previewRecording || busy();
    SolidBrush halo(running ? Color(42,255,112,112) : Color(24,173,214,210));
    g.FillEllipse(&halo,28,32,56,56);
    if (hover == 0) { Pen ring(Color(125,224,238,240),1); g.DrawEllipse(&ring,28,32,56,56); }
    SolidBrush button(running ? ui::danger : ui::accent);
    g.FillEllipse(&button,33,37,46,46);
    SolidBrush icon(Color(255,30,41,48));
    if (running) {
        GraphicsPath stop; rounded(stop,48,52,16,16,4); g.FillPath(&icon,&stop);
    } else {
        g.FillEllipse(&icon,47,51,18,18);
    }
    text(g,duration().c_str(),99,31,151,31,27,ui::primary,L"Segoe UI");
    std::wstring state = running ? L"录音中" : L"全局录音";
    if (recorder.state() == lightrec::CaptureState::Stopping) state=L"保存中";
    if (recorder.state() == lightrec::CaptureState::Saved) state=L"已保存";
    if (recorder.state() == lightrec::CaptureState::Failed) state=L"录音异常";
    text(g,state.c_str(),100,66,70,20,12,ui::secondary);
    // The pair of small meters represents the two recorded sources.
    for (int source=0; source<2; ++source) {
        float level = previewRecording ? (source ? .52f : .82f) :
            (source ? recorder.systemLevel() : recorder.micLevel());
        for (int bar=0; bar<5; ++bar) {
            SolidBrush meter(level > bar*.18f && running ?
                Color(230,167,209,204) : Color(65,150,178,186));
            g.FillRectangle(&meter,176.f+bar*5,71.f+source*7,3.f,3.f);
        }
    }
    bool on = enabled || previewRecording;
    GraphicsPath pill; rounded(pill,258,39,64,38,13);
    SolidBrush pillFill(on ? Color(34,170,219,208) : Color(16,214,230,236));
    g.FillPath(&pillFill,&pill);
    if (hover==1) { Pen border(Color(100,190,220,217),1); g.DrawPath(&border,&pill); }
    text(g,on?L"转写 开":L"转写 关",269,43,56,30,11,
         on?Color(255,190,225,218):Color(255,165,180,189));
    Color utility = on ? Color(255,199,214,222):Color(105,164,180,190);
    Pen pausePen(utility,2);
    if (paused) {
        PointF pts[]={{341,51},{341,65},{352,58}};
        SolidBrush b(utility); g.FillPolygon(&b,pts,3);
    } else {
        g.DrawLine(&pausePen,341,51,341,65); g.DrawLine(&pausePen,349,51,349,65);
    }
    if (hover == 2 || hover == 3) {
        GraphicsPath hp; rounded(hp,hover==2?328.f:376.f,39,38,38,12);
        SolidBrush hb(Color(18,210,229,238)); g.FillPath(&hb,&hp);
    }
    SolidBrush dot(Color(255,184,203,214));
    for(int i=0;i<3;++i) g.FillEllipse(&dot,386.f+i*6,56.f,3.f,3.f);
    std::wstring foot;
    if (!note.empty()) foot = note;
    else if (paused && enabled) foot=L"转写已暂停 · 录音照常";
    else {
        bool scoped=busy()?sessionProgramOnly:programOnly;
        const auto& name=busy()?sessionTarget:targetExe;
        foot = (scoped?fs::path(name).filename().wstring():L"全局声音") +
            L"  ·  " + (busy()?activeRoot:root).filename().wstring() + L"   ›";
    }
    text(g,foot.c_str(),34,87,380,18,10,Color(245,161,181,191));
}
void paintDockTab(Graphics& g, DockEdge edge, float opacity=1) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    // A 48px round badge is half hidden past the edge. Draw only the visible half.
    // Left/right share the recorder's circular identity rather than a scrollbar-like handle.
    float x=edge==DockEdge::Left?-24.f:0.f, y=edge==DockEdge::Top?-24.f:0.f;
    auto alpha=[&](int a){return (BYTE)std::lround(a*opacity);};
    SolidBrush body(Color(alpha(245),27,33,41));
    g.FillEllipse(&body,x,y,48.f,48.f);
    bool active=previewRecording||busy();
    BYTE r=active?244:167, green=active?117:217, b=active?118:209;
    Pen ring(Color(alpha(210),r,green,b),1.5f);
    g.DrawEllipse(&ring,x+1,y+1,46.f,46.f);
    bool vertical=edge==DockEdge::Left||edge==DockEdge::Right;
    float cx=vertical?(edge==DockEdge::Left?10.f:14.f):24.f;
    float cy=vertical?24.f:(edge==DockEdge::Top?10.f:14.f);
    SolidBrush dot(Color(alpha(255),r,green,b));
    g.FillEllipse(&dot,cx-3.5f,cy-3.5f,7.f,7.f);
}
bool savePng(Bitmap& bitmap,const fs::path& path) {
    UINT count=0,size=0; GetImageEncodersSize(&count,&size);
    std::vector<BYTE> buffer(size); auto enc=(ImageCodecInfo*)buffer.data();
    GetImageEncoders(count,size,enc);
    for(UINT i=0;i<count;++i)
        if(wcscmp(enc[i].MimeType,L"image/png")==0)
            return bitmap.Save(path.c_str(),&enc[i].Clsid,nullptr)==Ok;
    return false;
}
bool render(const fs::path& path, int tab=0) {
    bool vertical=tab==1||tab==3;
    int width=tab?(vertical?24:48):W, height=tab?(vertical?48:24):H;
    Bitmap bitmap(width*2,height*2,PixelFormat32bppPARGB);
    Graphics g(&bitmap); g.ScaleTransform(2,2);
    if(tab) {
        g.Clear(Color(0,0,0,0));
        paintDockTab(g,tab==1?DockEdge::Left:tab==2?DockEdge::Top:
            tab==3?DockEdge::Right:DockEdge::Bottom);
    } else paint(g);
    g.Flush(); return savePng(bitmap,path);
}
void redraw() {
    if (!window || IsIconic(window)) return;
    bool vertical=dockEdge==DockEdge::Left||dockEdge==DockEdge::Right;
    int w=(int)((dockCollapsed?(vertical?24:48):W)*dpi);
    int h=(int)((dockCollapsed?(vertical?48:24):H)*dpi);
    HDC screen=GetDC(nullptr), memory=CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=-h;
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;
    HBITMAP dib=CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);
    if (!dib) { DeleteDC(memory); ReleaseDC(nullptr,screen); return; }
    HGDIOBJ previous=SelectObject(memory,dib);
    {
        Bitmap bitmap(w,h,w*4,PixelFormat32bppPARGB,(BYTE*)pixels);
        Graphics g(&bitmap); g.ScaleTransform(dpi,dpi);
        if(dockCollapsed) {
            g.Clear(Color(0,0,0,0)); paintDockTab(g,dockEdge);
        } else if(dockAnimating) {
            float offset=1-dockReveal;
            auto state=g.Save();
            g.TranslateTransform(dockEdge==DockEdge::Left?-W*offset:
                dockEdge==DockEdge::Right?W*offset:0,
                dockEdge==DockEdge::Top?-H*offset:dockEdge==DockEdge::Bottom?H*offset:0);
            paint(g); g.Restore(state);
            // Render the badge at the same physical anchor it occupies when collapsed.
            LONG bw=(LONG)((vertical?24:48)*dpi),bh=(LONG)((vertical?48:24)*dpi);
            LONG bx=std::clamp((dockExpanded.left+dockExpanded.right-bw)/2,
                dockArea.left,std::max(dockArea.left,dockArea.right-bw));
            LONG by=std::clamp((dockExpanded.top+dockExpanded.bottom-bh)/2,
                dockArea.top,std::max(dockArea.top,dockArea.bottom-bh));
            if(dockEdge==DockEdge::Left) bx=dockArea.left;
            if(dockEdge==DockEdge::Right) bx=dockArea.right-bw;
            if(dockEdge==DockEdge::Top) by=dockArea.top;
            if(dockEdge==DockEdge::Bottom) by=dockArea.bottom-bh;
            g.TranslateTransform((bx-dockExpanded.left)/dpi,(by-dockExpanded.top)/dpi);
            paintDockTab(g,dockEdge,std::clamp((offset-.25f)/.75f,0.f,1.f));
        } else paint(g);
        g.Flush();
        if(diagnostic&&!diagnosticFrame.empty()) {
            savePng(bitmap,diagnosticFrame); diagnosticFrame.clear();
        }
    }
    SIZE size{w,h}; POINT src{}; BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    UpdateLayeredWindow(window,screen,nullptr,&size,memory,&src,0,&blend,ULW_ALPHA);
    SelectObject(memory,previous); DeleteObject(dib); DeleteDC(memory); ReleaseDC(nullptr,screen);
}
DockEdge edgeNear(const RECT& rect,const RECT& area,float scale) {
    LONG threshold=(LONG)std::lround(24*scale);
    // Crossing the edge still docks. Absolute-distance gating lost docking on overshoot,
    // then clamped the window to the edge without arming auto-hide.
    LONG distances[]={
        rect.left+(LONG)std::lround(12*scale)-area.left,
        area.right-rect.right+(LONG)std::lround(12*scale),
        rect.top+(LONG)std::lround(13*scale)-area.top,
        area.bottom-rect.bottom+(LONG)std::lround(19*scale)};
    int closest=-1;
    for(int i=0;i<4;++i)
        if(distances[i]<=threshold&&(closest<0||std::abs(distances[i])<std::abs(distances[closest])))
            closest=i;
    return closest>=0?(DockEdge)(closest+1):DockEdge::None;
}
RECT dockBounds(RECT rect,const RECT& area,float scale,DockEdge edge) {
    LONG width=rect.right-rect.left, height=rect.bottom-rect.top;
    LONG left=area.left-(LONG)std::lround(12*scale);
    LONG right=area.right+(LONG)std::lround(12*scale)-width;
    LONG top=area.top-(LONG)std::lround(13*scale);
    LONG bottom=area.bottom+(LONG)std::lround(19*scale)-height;
    rect.left=std::clamp(rect.left,left,std::max(left,right));
    rect.top=std::clamp(rect.top,top,std::max(top,bottom));
    if(edge==DockEdge::Left) rect.left=left;
    if(edge==DockEdge::Right) rect.left=right;
    if(edge==DockEdge::Top) rect.top=top;
    if(edge==DockEdge::Bottom) rect.top=bottom;
    rect.right=rect.left+width; rect.bottom=rect.top+height; return rect;
}
RECT tabBounds(const RECT& full,const RECT& area,float scale,DockEdge edge) {
    bool vertical=edge==DockEdge::Left||edge==DockEdge::Right;
    LONG width=(LONG)((vertical?24:48)*scale),height=(LONG)((vertical?48:24)*scale);
    LONG x=std::clamp((full.left+full.right-width)/2,area.left,std::max(area.left,area.right-width));
    LONG y=std::clamp((full.top+full.bottom-height)/2,area.top,std::max(area.top,area.bottom-height));
    if(edge==DockEdge::Left) x=area.left;
    if(edge==DockEdge::Right) x=area.right-width;
    if(edge==DockEdge::Top) y=area.top;
    if(edge==DockEdge::Bottom) y=area.bottom-height;
    return {x,y,x+width,y+height};
}
RECT draggedBounds(const RECT& origin,POINT start,POINT cursor) {
    LONG dx=cursor.x-start.x,dy=cursor.y-start.y;
    return {origin.left+dx,origin.top+dy,origin.right+dx,origin.bottom+dy};
}
bool shouldCollapse(bool collapsed,bool held,bool inside,ULONGLONG now,ULONGLONG since) {
    return !collapsed&&!held&&!inside&&since&&now-since>=650;
}
bool shouldExpandDock(bool inside,bool held,bool& armed) {
    if(!inside) armed=true;
    return inside&&!held&&armed;
}
void placeHud(const RECT& rect) {
    SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,
                 SWP_NOZORDER|SWP_NOACTIVATE);
    redraw();
}
void finishDockAnimation() {
    KillTimer(window,3); dockAnimating=false; dockReveal=dockHiding?0.f:1.f;
    dockCollapsed=dockHiding;
    if(dockCollapsed) {
        placeHud(tabBounds(dockExpanded,dockArea,dpi,dockEdge));
        POINT cursor; GetCursorPos(&cursor); RECT rect; GetWindowRect(window,&rect);
        dockHoverArmed=!PtInRect(&rect,cursor);
    } else placeHud(dockExpanded);
}
float dockAnimationValue(ULONGLONG elapsed,float from,bool hiding) {
    float t=std::min(1.f,elapsed/(float)(hiding?DockHideMs:DockShowMs));
    // Smoothstep has zero velocity at both ends; the old ease-out moved 87.5% in 80ms.
    float eased=t*t*(3-2*t);
    return from+((hiding?0.f:1.f)-from)*eased;
}
void advanceDockAnimation(ULONGLONG now) {
    if(!dockAnimating) return;
    dockReveal=dockAnimationValue(now-dockAnimationStart,dockAnimationFrom,dockHiding);
    if(now-dockAnimationStart>=(dockHiding?DockHideMs:DockShowMs)) finishDockAnimation(); else redraw();
}
bool dockAnimationAllowed() {
    BOOL enabled=TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&enabled,0);
    return enabled!=FALSE;
}
void beginDockAnimation(bool hiding) {
    dockAnimationFrom=dockReveal; dockHiding=hiding; dockAnimating=true;
    dockAnimationStart=GetTickCount64(); SetTimer(window,3,16,nullptr); redraw();
}
void expandDock(bool animate) {
    outsideSince=0;
    if(!dockCollapsed&&!dockAnimating) return;
    if(animate&&dockAnimationAllowed()) {
        if(dockAnimating&&!dockHiding) return;
        dockCollapsed=false; hover=pressed=-1;
        beginDockAnimation(false); placeHud(dockExpanded); return;
    }
    KillTimer(window,3); dockAnimating=dockCollapsed=false; dockReveal=1;
    hover=pressed=-1; placeHud(dockExpanded);
}
void collapseDock(bool animate=false) {
    if(dockEdge==DockEdge::None||dockCollapsed||dockAnimating) return;
    GetWindowRect(window,&dockExpanded);
    hover=pressed=-1; dockHiding=true;
    if(animate&&dockAnimationAllowed()) beginDockAnimation(true); else finishDockAnimation();
}
void finishDrag() {
    RECT rect; GetWindowRect(window,&rect);
    MONITORINFO info{sizeof(info)};
    if(GetMonitorInfoW(MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST),&info)) {
        dockArea=info.rcWork; dockEdge=edgeNear(rect,dockArea,dpi);
        dockExpanded=dockBounds(rect,dockArea,dpi,dockEdge); placeHud(dockExpanded);
    }
    outsideSince=0;
    if(dockEdge!=DockEdge::None) SetTimer(window,2,80,nullptr); else KillTimer(window,2);
    // Like an edge-docked ball: releasing at the edge retracts immediately, even if
    // the cursor is still over the expanded panel. Hover must leave/re-enter the badge.
    if(dockEdge!=DockEdge::None) collapseDock(true);
}
void rememberPosition() {
    if(diagnostic || !window || IsIconic(window)) return;
    RECT rect=dockExpanded; if(!dockCollapsed&&!dockAnimating) GetWindowRect(window,&rect);
    setting(L"x",std::to_wstring(rect.left)); setting(L"y",std::to_wstring(rect.top));
    setting(L"dock_edge",std::to_wstring((int)dockEdge));
}
void chooseFolder() {
    IFileOpenDialog* dialog=nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog)))) return;
    DWORD opts=0; dialog->GetOptions(&opts);
    dialog->SetOptions(opts|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
    dialog->SetTitle(busy()?L"选择下一次录音的保存文件夹":L"选择录音保存文件夹");
    IShellItem* initial=nullptr;
    if(SUCCEEDED(SHCreateItemFromParsingName(root.c_str(),nullptr,IID_PPV_ARGS(&initial)))) {
        dialog->SetFolder(initial); initial->Release();
    }
    if(SUCCEEDED(dialog->Show(window))) {
        IShellItem* item=nullptr;
        if(SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR name=nullptr;
            if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&name))) {
                fs::path selected(name); CoTaskMemFree(name);
                // Verify write access with a unique file, never touch any existing recording.
                fs::path probe=selected/(L".lightrec-write-"+std::to_wstring(GetCurrentProcessId())+
                                         L"-"+std::to_wstring(GetTickCount64())+L".tmp");
                HANDLE test=CreateFileW(probe.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                                       FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
                if(test==INVALID_HANDLE_VALUE) note=L"该文件夹无法写入，请换一个位置";
                else {
                    CloseHandle(test);
                    if(setting(L"folder",selected.wstring())) {
                        root=selected;
                        note=busy()?L"位置已记住 · 下一次录音生效":L"保存位置已记住";
                    } else note=L"保存设置失败，位置未更改";
                }
            }
            item->Release();
        }
    }
    dialog->Release(); redraw();
}
void filterPrograms() {
    wchar_t filter[260]{}; GetWindowTextW(programSearch,filter,260);
    std::wstring needle=filter;
    std::transform(needle.begin(),needle.end(),needle.begin(),towlower);
    SendMessageW(programList,LB_RESETCONTENT,0,0); filteredPrograms.clear();
    for(size_t i=0;i<programs.size();++i) {
        std::wstring name=programs[i].name;
        std::transform(name.begin(),name.end(),name.begin(),towlower);
        if(!needle.empty()&&name.find(needle)==std::wstring::npos) continue;
        std::wstring label=programs[i].name+L"   ·   PID "+std::to_wstring(programs[i].pid);
        SendMessageW(programList,LB_ADDSTRING,0,(LPARAM)label.c_str());
        filteredPrograms.push_back(i);
    }
    if(!filteredPrograms.empty()) SendMessageW(programList,LB_SETCURSEL,0,0);
}
void selectedProgram(const std::wstring& path,DWORD pid) {
    if(!setting(L"target_exe",path)||!setting(L"program_only",L"1")) {
        note=L"保存设置失败，请重新选择"; return;
    }
    setting(L"preferred_pid",std::to_wstring(pid));
    targetExe=path; preferredPid=pid; programOnly=true;
    callHits=0; callLatched=false;
    note=busy()?L"监听范围已记住 · 下一次录音生效":L"已选择 "+fs::path(path).filename().wstring();
}
void layoutPicker(HWND hwnd) {
    float scale=GetDpiForWindow(hwnd)/96.f;
    struct Layout { int id,x,y,w,h; };
    const Layout controls[]={{501,16,16,434,30},{502,16,58,434,274},
        {503,16,347,132,32},{IDCANCEL,250,347,90,32},{IDOK,350,347,100,32}};
    HFONT next=CreateFontW(-(int)(14*scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    for(const auto& c:controls) {
        HWND child=GetDlgItem(hwnd,c.id);
        MoveWindow(child,(int)(c.x*scale),(int)(c.y*scale),(int)(c.w*scale),(int)(c.h*scale),TRUE);
        SendMessageW(child,WM_SETFONT,(WPARAM)next,TRUE);
    }
    if(pickerFont) DeleteObject(pickerFont); pickerFont=next;
}
LRESULT CALLBACK pickerProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_DPICHANGED) {
        RECT* rect=(RECT*)lp;
        SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,
                     SWP_NOZORDER|SWP_NOACTIVATE);
        layoutPicker(hwnd); return 0;
    }
    if(msg==WM_COMMAND) {
        int id=LOWORD(wp), notification=HIWORD(wp);
        if(id==501&&notification==EN_CHANGE) { filterPrograms(); return 0; }
        if(id==IDOK || (id==502&&notification==LBN_DBLCLK)) {
            int at=(int)SendMessageW(programList,LB_GETCURSEL,0,0);
            if(at>=0 && (size_t)at<filteredPrograms.size()) {
                const auto& item=programs[filteredPrograms[at]];
                selectedProgram(item.path,item.pid); DestroyWindow(hwnd);
            }
            return 0;
        }
        if(id==503) {
            IFileOpenDialog* dialog=nullptr;
            if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) {
                COMDLG_FILTERSPEC filter{L"应用程序 (*.exe)",L"*.exe"};
                dialog->SetFileTypes(1,&filter); dialog->SetTitle(L"选择要监听的程序");
                if(SUCCEEDED(dialog->Show(hwnd))) {
                    IShellItem* item=nullptr; PWSTR path=nullptr;
                    if(SUCCEEDED(dialog->GetResult(&item))) {
                        if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) {
                            selectedProgram(path,0); CoTaskMemFree(path); DestroyWindow(hwnd);
                        }
                        item->Release();
                    }
                }
                dialog->Release();
            }
            return 0;
        }
        if(id==IDCANCEL) { DestroyWindow(hwnd); return 0; }
    }
    if(msg==WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if(msg==WM_DESTROY) {
        EnableWindow(window,TRUE); picker=nullptr; programList=nullptr; programSearch=nullptr;
        if(pickerFont) { DeleteObject(pickerFont); pickerFont=nullptr; }
        ShowWindow(window,SW_SHOWNORMAL); SetForegroundWindow(window); redraw(); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void chooseProgram() {
    if(!lightrec::processLoopbackSupported()) {
        MessageBoxW(window,L"按程序录音需要 Windows 版本 20348 或更高。\n仍可使用全局录音。",
                    L"全局录音",MB_OK|MB_ICONINFORMATION); return;
    }
    if(picker) { SetForegroundWindow(picker); return; }
    programs=runningPrograms();
    picker=CreateWindowExW(WS_EX_TOOLWINDOW,L"LightRecProgram",L"选择监听的程序",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, CW_USEDEFAULT,CW_USEDEFAULT,(int)(484*dpi),(int)(440*dpi),
        window,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!picker) { note=L"程序选择窗口创建失败"; return; }
    programSearch=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
        16,16,434,30,picker,(HMENU)501,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(programSearch,EM_SETCUEBANNER,TRUE,(LPARAM)L"搜索程序名，例如 Weixin、WeChat、msedge");
    programList=CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",L"",
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
        16,58,434,274,picker,(HMENU)502,GetModuleHandleW(nullptr),nullptr);
    auto addButton=[&](int id,const wchar_t* name,int x,int width) {
        HWND button=CreateWindowExW(0,L"BUTTON",name,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
            x,347,width,32,picker,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(button,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    };
    addButton(503,L"从文件选择…",16,132); addButton(IDCANCEL,L"取消",250,90); addButton(IDOK,L"选择",350,100);
    SendMessageW(programSearch,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    SendMessageW(programList,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    layoutPicker(picker); filterPrograms();
    EnableWindow(window,FALSE); ShowWindow(picker,SW_SHOW); SetFocus(programSearch);
}
void updateText() {
    if(!edit || session.empty()) return;
    std::ifstream in(session/L"转写.txt",std::ios::binary);
    std::string value((std::istreambuf_iterator<char>(in)),{});
    if(value.rfind("\xef\xbb\xbf",0)==0) value.erase(0,3);
    auto w=wide(value);
    if(w.empty()) w=enabled?L"转写内容将在音频分段处理后显示。":L"开启转写后，这里会显示本地识别的文字。";
    std::wstring crlf;
    for(wchar_t c:w) { if(c==L'\n') crlf+=L'\r'; crlf+=c; }
    if(crlf!=transcriptCache) { SetWindowTextW(edit,crlf.c_str()); transcriptCache=crlf; }
}
LRESULT CALLBACK textProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_SIZE && edit) { MoveWindow(edit,16,16,LOWORD(lp)-32,HIWORD(lp)-32,TRUE); return 0; }
    if(msg==WM_CLOSE) { ShowWindow(hwnd,SW_HIDE); expanded=false; return 0; }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void showText() {
    if(!transcript) {
        transcript=CreateWindowExW(WS_EX_TOOLWINDOW,L"LightRecText",L"全局录音 · 转写",
            WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,620,380,window,nullptr,GetModuleHandleW(nullptr),nullptr);
        edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            16,16,572,308,transcript,nullptr,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(edit,EM_SETLIMITTEXT,2*1024*1024,0);
        SendMessageW(edit,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    }
    expanded=true; updateText(); ShowWindow(transcript,SW_SHOW); SetForegroundWindow(transcript);
}
bool monitoredActive() {
    IMMDeviceEnumerator* enumerator=nullptr; IMMDeviceCollection* devices=nullptr;
    bool found=false;
    if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator))) &&
       SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture,DEVICE_STATE_ACTIVE,&devices))) {
      UINT deviceCount=0; devices->GetCount(&deviceCount);
      for(UINT index=0;index<deviceCount&&!found;++index) {
       IMMDevice* device=nullptr; IAudioSessionManager2* manager=nullptr;
       IAudioSessionEnumerator* sessions=nullptr;
       if(SUCCEEDED(devices->Item(index,&device)) &&
          SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,(void**)&manager)) &&
          SUCCEEDED(manager->GetSessionEnumerator(&sessions))) {
        int count=0; sessions->GetCount(&count);
        for(int i=0;i<count&&!found;++i) {
            IAudioSessionControl* control=nullptr; IAudioSessionControl2* detail=nullptr;
            if(SUCCEEDED(sessions->GetSession(i,&control))) {
                AudioSessionState state; DWORD pid=0;
                if(SUCCEEDED(control->GetState(&state)) && state==AudioSessionStateActive &&
                   SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&detail))) &&
                   SUCCEEDED(detail->GetProcessId(&pid))) {
                    if(!programOnly && pid!=GetCurrentProcessId() && pid!=0) found=true;
                    HANDLE process=programOnly?OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid):nullptr;
                    if(process) {
                        wchar_t path[32768]; DWORD length=32768;
                        if(QueryFullProcessImageNameW(process,0,path,&length)) {
                            found=_wcsicmp(path,targetExe.c_str())==0;
                        }
                        CloseHandle(process);
                    }
                }
                if(detail) detail->Release(); control->Release();
            }
        }
       }
       if(sessions) sessions->Release(); if(manager) manager->Release();
       if(device) device->Release();
      }
    }
    if(devices) devices->Release(); if(enumerator) enumerator->Release();
    return found;
}
void checkCall() {
    if(!callMode || diagnostic || picker || hotkeyDialog) return;
    bool active=monitoredActive();
    if(!active) { callHits=0; callLatched=false; return; }
    if(++callHits<3 || callLatched) return;
    callLatched=true;
    if(busy()) return;
    if(callMode==2 || MessageBoxW(window,
        L"检测到监听范围内的程序正在使用麦克风，是否开始录音？\n语音消息、语音输入也可能触发此提示。",
        L"全局录音",MB_YESNO|MB_ICONQUESTION)==IDYES) recordToggle();
}
bool sameKey(Shortcut a,Shortcut b) { return a.key==b.key&&a.modifiers==b.modifiers; }
bool validKey(Shortcut value) {
    if(value.modifiers&~(MOD_CONTROL|MOD_ALT|MOD_SHIFT)) return false;
    bool function=value.key>=VK_F1&&value.key<=VK_F24;
    bool regular=(value.key>='A'&&value.key<='Z')||(value.key>='0'&&value.key<='9')||
        (value.key>=VK_PRIOR&&value.key<=VK_DOWN)||value.key==VK_SPACE||
        value.key==VK_INSERT||value.key==VK_DELETE||value.key==VK_PAUSE||
        (value.key>=VK_OEM_1&&value.key<=VK_OEM_3)||(value.key>=VK_OEM_4&&value.key<=VK_OEM_8);
    return function || (regular&&(value.modifiers&(MOD_CONTROL|MOD_ALT)));
}
WORD controlKey(Shortcut value) {
    BYTE modifiers=0;
    if(value.modifiers&MOD_CONTROL) modifiers|=HOTKEYF_CONTROL;
    if(value.modifiers&MOD_ALT) modifiers|=HOTKEYF_ALT;
    if(value.modifiers&MOD_SHIFT) modifiers|=HOTKEYF_SHIFT;
    if((value.key>=VK_PRIOR&&value.key<=VK_DOWN)||value.key==VK_INSERT||value.key==VK_DELETE)
        modifiers|=HOTKEYF_EXT;
    return MAKEWORD((BYTE)value.key,modifiers);
}
Shortcut fromControl(WORD value) {
    UINT modifiers=0; BYTE flags=HIBYTE(value);
    if(flags&HOTKEYF_CONTROL) modifiers|=MOD_CONTROL;
    if(flags&HOTKEYF_ALT) modifiers|=MOD_ALT;
    if(flags&HOTKEYF_SHIFT) modifiers|=MOD_SHIFT;
    return {modifiers,LOBYTE(value)};
}
std::wstring keyName(Shortcut value) {
    std::wstring name;
    if(value.modifiers&MOD_CONTROL) name+=L"Ctrl+";
    if(value.modifiers&MOD_ALT) name+=L"Alt+";
    if(value.modifiers&MOD_SHIFT) name+=L"Shift+";
    if(value.key>=VK_F1&&value.key<=VK_F24) return name+L"F"+std::to_wstring(value.key-VK_F1+1);
    wchar_t label[80]{};
    UINT scan=MapVirtualKeyW(value.key,MAPVK_VK_TO_VSC);
    if(value.key>=VK_PRIOR&&value.key<=VK_DOWN) scan|=0x100;
    GetKeyNameTextW((LONG)(scan<<16),label,80);
    return name+(label[0]?std::wstring(label):std::to_wstring(value.key));
}
bool saveKeys(Shortcut first,Shortcut second) {
    return setting(L"record_modifiers",std::to_wstring(first.modifiers))&&
        setting(L"record_key",std::to_wstring(first.key))&&
        setting(L"pause_modifiers",std::to_wstring(second.modifiers))&&
        setting(L"pause_key",std::to_wstring(second.key));
}
using RegisterBinding=std::function<bool(int,Shortcut)>;
using UnregisterBinding=std::function<void(int)>;
bool replaceKeys(Shortcut first,Shortcut second,std::wstring& issue,
                 const RegisterBinding& reg,const UnregisterBinding& unreg) {
    if(!validKey(first)||!validKey(second)) {
        issue=L"请使用 Ctrl / Alt 组合键，或 F1–F24。"; return false;
    }
    if(sameKey(first,second)) { issue=L"录音与暂停转写不能使用相同快捷键。"; return false; }
    Shortcut oldFirst=recordKey,oldSecond=pauseKey;
    unreg(1); unreg(2);
    bool firstOK=reg(1,first), secondOK=reg(2,second);
    bool persisted=false;
    if(firstOK&&secondOK) persisted=saveKeys(first,second);
    if(!firstOK||!secondOK||!persisted) {
        if(firstOK) unreg(1); if(secondOK) unreg(2);
        hotkeyR=reg(1,oldFirst); hotkeyT=reg(2,oldSecond);
        if(firstOK&&secondOK) saveKeys(oldFirst,oldSecond);
        issue=firstOK&&secondOK?L"设置保存失败，保留原来的快捷键。":
            L"快捷键被占用或被系统保留，请换一个组合。";
        if(!hotkeyR||!hotkeyT) issue+=L" 原快捷键也有占用，可换键后保存。";
        return false;
    }
    recordKey=first; pauseKey=second; hotkeyR=hotkeyT=true; issue.clear(); return true;
}
bool registerBinding(int id,Shortcut key) {
    return RegisterHotKey(window,id,key.modifiers|MOD_NOREPEAT,key.key)!=FALSE;
}
void unregisterBinding(int id) { UnregisterHotKey(window,id); }
void suspendKeys() {
    if(hotkeyR) unregisterBinding(1);
    if(hotkeyT) unregisterBinding(2);
    hotkeyR=hotkeyT=false;
}
HFONT hotkeyFont=nullptr;
bool keysSaved=false, keysSuspended=false;
void layoutKeys(HWND hwnd) {
    float scale=GetDpiForWindow(hwnd)/96.f;
    struct Item {int id,x,y,w,h;};
    Item items[]={{601,16,20,132,32},{602,160,20,290,32},{603,16,68,132,32},
        {604,160,68,290,32},{605,16,112,434,48},{606,16,180,132,32},
        {IDCANCEL,250,180,90,32},{IDOK,350,180,100,32}};
    HFONT next=CreateFontW(-(int)(14*scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    for(const auto& item:items) {
        HWND child=GetDlgItem(hwnd,item.id);
        MoveWindow(child,(int)(item.x*scale),(int)(item.y*scale),(int)(item.w*scale),(int)(item.h*scale),TRUE);
        SendMessageW(child,WM_SETFONT,(WPARAM)next,TRUE);
    }
    if(hotkeyFont) DeleteObject(hotkeyFont); hotkeyFont=next;
}
LRESULT CALLBACK keysProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_COMMAND) {
        int id=LOWORD(wp);
        if(id==606) {
            SendMessageW(recordKeyInput,HKM_SETHOTKEY,controlKey({MOD_CONTROL|MOD_ALT,'R'}),0);
            SendMessageW(pauseKeyInput,HKM_SETHOTKEY,controlKey({MOD_CONTROL|MOD_ALT,'T'}),0);
            return 0;
        }
        if(id==IDOK) {
            auto first=fromControl((WORD)SendMessageW(recordKeyInput,HKM_GETHOTKEY,0,0));
            auto second=fromControl((WORD)SendMessageW(pauseKeyInput,HKM_GETHOTKEY,0,0));
            std::wstring issue;
            if(replaceKeys(first,second,issue,registerBinding,unregisterBinding)) {
                keysSaved=true; note=L"快捷键已保存，立即生效"; DestroyWindow(hwnd);
            } else {
                // Keep keystrokes in the editor from triggering recording.
                suspendKeys(); SetDlgItemTextW(hwnd,605,issue.c_str());
            }
            return 0;
        }
        if(id==IDCANCEL) { DestroyWindow(hwnd); return 0; }
    }
    if(msg==WM_DPICHANGED) {
        RECT* r=(RECT*)lp;
        SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
        layoutKeys(hwnd); return 0;
    }
    if(msg==WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if(msg==WM_DESTROY) {
        if(!keysSaved&&keysSuspended) {
            hotkeyR=registerBinding(1,recordKey); hotkeyT=registerBinding(2,pauseKey);
            if(!hotkeyR||!hotkeyT) note=L"快捷键被占用，可重新设置；圆钮录音可用";
        }
        keysSuspended=false;
        hotkeyDialog=nullptr; recordKeyInput=pauseKeyInput=nullptr;
        if(hotkeyFont) { DeleteObject(hotkeyFont); hotkeyFont=nullptr; }
        EnableWindow(window,TRUE); SetForegroundWindow(window); redraw(); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void editKeys() {
    if(hotkeyDialog) { SetForegroundWindow(hotkeyDialog); return; }
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_HOTKEY_CLASS};
    InitCommonControlsEx(&controls);
    hotkeyDialog=CreateWindowExW(WS_EX_TOOLWINDOW,L"LightRecKeys",L"快捷键设置",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,(int)(484*dpi),(int)(270*dpi),
        window,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!hotkeyDialog) { note=L"快捷键窗口创建失败"; return; }
    auto child=[&](int id,const wchar_t* type,const wchar_t* label,DWORD style) {
        return CreateWindowExW(0,type,label,WS_CHILD|WS_VISIBLE|style,
            0,0,1,1,hotkeyDialog,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
    };
    child(601,L"STATIC",L"开始 / 停止录音",SS_CENTERIMAGE);
    recordKeyInput=child(602,HOTKEY_CLASSW,L"",WS_BORDER|WS_TABSTOP);
    child(603,L"STATIC",L"暂停 / 恢复转写",SS_CENTERIMAGE);
    pauseKeyInput=child(604,HOTKEY_CLASSW,L"",WS_BORDER|WS_TABSTOP);
    if(!recordKeyInput||!pauseKeyInput) {
        keysSaved=false; DestroyWindow(hotkeyDialog); note=L"快捷键控件创建失败"; return;
    }
    SendMessageW(recordKeyInput,HKM_SETRULES,0,0); SendMessageW(pauseKeyInput,HKM_SETRULES,0,0);
    SendMessageW(recordKeyInput,HKM_SETHOTKEY,controlKey(recordKey),0);
    SendMessageW(pauseKeyInput,HKM_SETHOTKEY,controlKey(pauseKey),0);
    child(605,L"STATIC",L"点击输入框后按组合键；保存后立即生效。",SS_LEFT);
    child(606,L"BUTTON",L"恢复默认",WS_TABSTOP|BS_PUSHBUTTON);
    child(IDCANCEL,L"BUTTON",L"取消",WS_TABSTOP|BS_PUSHBUTTON);
    child(IDOK,L"BUTTON",L"保存",WS_TABSTOP|BS_DEFPUSHBUTTON);
    keysSaved=false; suspendKeys(); keysSuspended=true; layoutKeys(hotkeyDialog);
    EnableWindow(window,FALSE); ShowWindow(hotkeyDialog,SW_SHOW); SetFocus(recordKeyInput);
}
void prepareTrayData() {
    bool recording=busy();
    tray.cbSize=sizeof(tray); tray.hWnd=window; tray.uID=1;
    tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP|NIF_SHOWTIP; tray.uCallbackMessage=WM_TRAY;
    tray.hIcon=recording?trayRecordingIcon:trayIdleIcon;
    if(!tray.hIcon) tray.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    std::wstring tip=recording?L"全局录音 · 录音中 "+duration():L"全局录音 · 待机";
    if(enabled) tip+=paused?L" · 转写暂停":L" · 本地转写";
    wcsncpy_s(tray.szTip,tip.c_str(),_TRUNCATE);
}
bool installTray(bool force=false) {
    if(diagnostic) return false;
    if(trayAdded) {
        if(!force) return true;
        prepareTrayData();
        if(Shell_NotifyIconW(NIM_MODIFY,&tray)) { lastTrayTip=tray.szTip; return true; }
        trayAdded=false;
    }
    auto now=GetTickCount64();
    if(!force&&trayLastAttempt&&now-trayLastAttempt<5000) return false;
    trayLastAttempt=now; prepareTrayData();
    if(!Shell_NotifyIconW(NIM_ADD,&tray)&&!Shell_NotifyIconW(NIM_MODIFY,&tray)) return false;
    trayAdded=true; tray.uVersion=NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION,&tray);
    lastTrayTip=tray.szTip; return true;
}
void refreshTray() {
    if(diagnostic) return;
    if(!trayAdded) { installTray(); return; }
    prepareTrayData();
    if(lastTrayTip==tray.szTip) return;
    if(!Shell_NotifyIconW(NIM_MODIFY,&tray)) { trayAdded=false; return; }
    lastTrayTip=tray.szTip;
}
void toggleStartup() {
    std::wstring command;
    LONG status=lightrec::readStartup(command);
    if(status!=ERROR_SUCCESS&&status!=ERROR_FILE_NOT_FOUND) {
        note=L"读取自启动设置失败："+std::to_wstring(status); return;
    }
    bool turnOn=status!=ERROR_SUCCESS||command.empty();
    status=lightrec::writeStartup(turnOn,currentExecutable);
    if(status!=ERROR_SUCCESS) {
        note=L"修改自启动失败："+std::to_wstring(status); return;
    }
    status=lightrec::readStartup(command);
    bool confirmed=turnOn?
        status==ERROR_SUCCESS&&command==lightrec::startupCommand(currentExecutable):
        status==ERROR_FILE_NOT_FOUND;
    note=confirmed?(turnOn?L"已开启自启动 · 登录后在托盘待机":L"已关闭开机自启动"):
        L"自启动写入后核对失败，请重试";
}
enum class ExitStep { Confirm, Save, Destroy };
ExitStep exitStep(bool active,bool confirmed) {
    return active?(confirmed?ExitStep::Save:ExitStep::Confirm):ExitStep::Destroy;
}
CloseAction loadCloseAction() {
    auto value=ini(L"close_action",L"ask");
    return value==L"tray"?CloseAction::Tray:value==L"exit"?CloseAction::Exit:CloseAction::Ask;
}
bool rememberCloseChoice(CloseAction action,bool remember) {
    if(!remember||action==CloseAction::Ask) return true;
    if(!setting(L"close_action",action==CloseAction::Tray?L"tray":L"exit")) return false;
    closeAction=action; return true;
}
template<class EnsureTray,class Hide>
bool hideWithTray(EnsureTray ensureTray,Hide hide) {
    if(!ensureTray()) return false;
    hide(); return true;
}
bool hideToTray() {
    // A failed tray registration must never remove the only available control surface.
    bool hidden=hideWithTray([]{return installTray(true);},[]{
        rememberPosition(); ShowWindow(window,SW_HIDE);
    });
    if(!hidden) {
        note=L"托盘暂不可用，已保留悬浮条；稍后自动重试"; return false;
    }
    return true;
}
void requestExit(bool confirmed) {
    if(quitWhenSaved) return;
    switch(exitStep(busy(),confirmed)) {
    case ExitStep::Confirm:
        showCloseChoice(true); break;
    case ExitStep::Save:
        quitWhenSaved=true; recorder.stop(); note=L"正在保存，完成后退出"; flags(); break;
    case ExitStep::Destroy: DestroyWindow(window); break;
    }
}
void paintCloseBackground(HWND hwnd,HDC dc) {
    RECT rect; GetClientRect(hwnd,&rect);
    float scale=closeScale;
    Graphics g(dc); g.ScaleTransform(scale,scale); g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(ui::panel);
    GraphicsPath border; rounded(border,.5f,.5f,rect.right/scale-1,rect.bottom/scale-1,20);
    Pen stroke(ui::border,1); g.DrawPath(&stroke,&border);
    Pen wave(ui::accent,2);
    const float heights[]={8,14,20,12,6};
    for(int i=0;i<5;++i) g.DrawLine(&wave,25.f+i*4,34.f-heights[i]/2,25.f+i*4,34.f+heights[i]/2);
}
void paintCloseControl(HWND hwnd,HDC dc,bool hovered,bool forceFocus=false) {
    RECT rect; GetClientRect(hwnd,&rect);
    float scale=closeScale,w=rect.right/scale,h=rect.bottom/scale;
    Graphics g(dc); g.ScaleTransform(scale,scale); g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit); g.Clear(ui::panel);
    wchar_t label[256]{}; GetWindowTextW(hwnd,label,256);
    int id=GetDlgCtrlID(hwnd); bool focused=forceFocus||GetFocus()==hwnd;
    bool pushed=(SendMessageW(hwnd,BM_GETSTATE,0,0)&BST_PUSHED)!=0;
    if(id==700||id==704||id==705) {
        text(g,label,0,0,w,h,id==700?17.f:id==705?12.f:11.f,
            id==700?ui::primary:(id==704&&closeIssue?ui::danger:ui::secondary),ui::font,id==700);
        return;
    }
    if(id==703) {
        bool checked=SendMessageW(hwnd,BM_GETCHECK,0,0)==BST_CHECKED;
        GraphicsPath box; rounded(box,1,6,16,16,4);
        SolidBrush fill(checked?ui::accent:Color(hovered?35:12,194,225,222)); g.FillPath(&fill,&box);
        Pen edge(checked||hovered?ui::accent:Color(125,160,179,188),1); g.DrawPath(&edge,&box);
        if(checked) {
            Pen check(ui::panel,1.8f);
            g.DrawLine(&check,5.f,14.f,8.f,17.f); g.DrawLine(&check,8.f,17.f,13.f,11.f);
        }
        text(g,label,27,0,w-29,h,12,ui::secondary);
    } else if(id==706) {
        if(hovered||pushed) {
            GraphicsPath bg; rounded(bg,1,1,w-2,h-2,8);
            SolidBrush fill(Color(pushed?160:100,187,67,77)); g.FillPath(&fill,&bg);
        }
        Pen cross(hovered?ui::primary:ui::secondary,1.5f);
        g.DrawLine(&cross,w/2-4,h/2-4,w/2+4,h/2+4);
        g.DrawLine(&cross,w/2+4,h/2-4,w/2-4,h/2+4);
    } else {
        bool danger=id==702;
        bool primary=id==701;
        Color ink=danger?ui::danger:primary?ui::accent:ui::secondary;
        Color fill=danger?Color(pushed?65:hovered?45:20,244,117,118):
            primary?Color(pushed?65:hovered?48:34,170,219,208):Color(hovered?25:8,195,213,225);
        GraphicsPath bg; rounded(bg,1,1,w-2,h-2,12);
        SolidBrush brush(fill); g.FillPath(&brush,&bg);
        Pen border(hovered?ink:Color(danger?55:primary?65:35,ink.GetR(),ink.GetG(),ink.GetB()),1);
        g.DrawPath(&border,&bg);
        Font font(ui::font,12,FontStyleRegular,UnitPixel);
        SolidBrush textBrush(ink); StringFormat format;
        format.SetAlignment(StringAlignmentCenter); format.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(label,-1,&font,RectF(1,0,w-2,h),&format,&textBrush);
    }
    if(focused) {
        GraphicsPath focus; rounded(focus,1.5f,1.5f,w-3,h-3,id==703?6.f:10.f);
        Pen outline(Color(190,194,225,222),1); g.DrawPath(&outline,&focus);
    }
}
LRESULT CALLBACK closeControlProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR state) {
    if(msg==WM_NCHITTEST&&GetDlgCtrlID(hwnd)==700) return HTTRANSPARENT;
    if(msg==WM_SETCURSOR&&GetDlgCtrlID(hwnd)!=700&&GetDlgCtrlID(hwnd)!=704&&GetDlgCtrlID(hwnd)!=705) {
        SetCursor(LoadCursorW(nullptr,IDC_HAND)); return TRUE;
    }
    if(msg==WM_ERASEBKGND) return 1;
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps);
        paintCloseControl(hwnd,dc,(state&1)!=0,(state&2)!=0); EndPaint(hwnd,&ps); return 0;
    }
    if(msg==WM_PRINT||msg==WM_PRINTCLIENT) {
        paintCloseControl(hwnd,(HDC)wp,(state&1)!=0,(state&2)!=0); return 0;
    }
    if(msg==WM_MOUSEMOVE) {
        SetWindowSubclass(hwnd,closeControlProc,id,state|1);
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0}; TrackMouseEvent(&track);
    }
    if(msg==WM_MOUSELEAVE) SetWindowSubclass(hwnd,closeControlProc,id,state&~(DWORD_PTR)1);
    if(msg==WM_NCDESTROY) RemoveWindowSubclass(hwnd,closeControlProc,id);
    LRESULT result=DefSubclassProc(hwnd,msg,wp,lp);
    if(IsWindow(hwnd)&&(msg==WM_MOUSEMOVE||msg==WM_MOUSELEAVE||msg==WM_LBUTTONDOWN||msg==WM_LBUTTONUP||
       msg==WM_SETFOCUS||msg==WM_KILLFOCUS||msg==WM_KEYDOWN||msg==WM_KEYUP||
       msg==BM_SETCHECK||msg==WM_ENABLE||msg==WM_SETTEXT))
        InvalidateRect(hwnd,nullptr,FALSE);
    return result;
}
void layoutClose(HWND hwnd) {
    float scale=closeScale;
    struct Item { int id,x,y,w,h; };
    const Item items[]={{700,56,19,300,30},{705,24,64,376,23},{704,24,88,376,24},
        {703,24,124,260,28},{701,24,176,162,40},{702,198,176,112,40},
        {IDCANCEL,322,176,78,40},{706,368,15,32,32}};
    HFONT next=CreateFontW(-(int)(14*scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    for(const auto& item:items) {
        HWND child=GetDlgItem(hwnd,item.id);
        int y=closeExitOnly&&(item.id==702||item.id==IDCANCEL)?136:item.y;
        MoveWindow(child,(int)(item.x*scale),(int)(y*scale),(int)(item.w*scale),(int)(item.h*scale),TRUE);
        SendMessageW(child,WM_SETFONT,(WPARAM)next,TRUE);
    }
    if(closeFont) DeleteObject(closeFont); closeFont=next;
    RECT client; GetClientRect(hwnd,&client);
    HRGN region=CreateRoundRectRgn(0,0,client.right+1,client.bottom+1,(int)(40*scale),(int)(40*scale));
    if(!SetWindowRgn(hwnd,region,TRUE)) DeleteObject(region);
    InvalidateRect(hwnd,nullptr,FALSE);
}
LRESULT CALLBACK closeProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_ERASEBKGND) return 1;
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps);
        paintCloseBackground(hwnd,dc); EndPaint(hwnd,&ps); return 0;
    }
    if(msg==WM_PRINTCLIENT) { paintCloseBackground(hwnd,(HDC)wp); return 0; }
    if(msg==DM_GETDEFID) return MAKELONG(closeExitOnly?702:701,DC_HASDEFID);
    if(msg==WM_LBUTTONDOWN&&GET_Y_LPARAM(lp)<(int)(54*GetDpiForWindow(hwnd)/96.f)) {
        POINT cursor; GetCursorPos(&cursor);
        ReleaseCapture(); SendMessageW(hwnd,WM_NCLBUTTONDOWN,HTCAPTION,MAKELPARAM(cursor.x,cursor.y)); return 0;
    }
    if(msg==WM_DPICHANGED) {
        closeScale=HIWORD(wp)/96.f;
        RECT* rect=(RECT*)lp;
        SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,
                     SWP_NOZORDER|SWP_NOACTIVATE);
        layoutClose(hwnd); return 0;
    }
    if(msg==WM_COMMAND) {
        int id=LOWORD(wp);
        if(id==IDOK) id=closeExitOnly?702:701;
        if(id==701||id==702) {
            CloseAction choice=id==701?CloseAction::Tray:CloseAction::Exit;
            bool remember=!closeExitOnly&&IsDlgButtonChecked(hwnd,703)==BST_CHECKED;
            if(choice==CloseAction::Tray&&!installTray(true)) {
                closeIssue=true; SetDlgItemTextW(hwnd,704,L"托盘暂不可用，请稍后重试，或选择退出程序。"); return 0;
            }
            if(!rememberCloseChoice(choice,remember)) {
                closeIssue=true; SetDlgItemTextW(hwnd,704,L"无法保存选择，请取消勾选后重试。"); return 0;
            }
            DestroyWindow(hwnd);
            if(choice==CloseAction::Tray) hideToTray(); else requestExit(true);
            return 0;
        }
        if(id==IDCANCEL||id==706) { DestroyWindow(hwnd); return 0; }
    }
    if(msg==WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if(msg==WM_DESTROY) {
        closeDialog=nullptr;
        if(closeFont) { DeleteObject(closeFont); closeFont=nullptr; }
        EnableWindow(window,TRUE); if(!diagnostic) SetForegroundWindow(window); outsideSince=0;
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void showCloseChoice(bool exitOnly) {
    if(closeDialog) { SetForegroundWindow(closeDialog); return; }
    expandDock();
    closeExitOnly=exitOnly; closeIssue=false;
    float scale=GetDpiForWindow(window)/96.f;
    closeScale=scale;
    RECT bounds{0,0,(LONG)(424*scale),(LONG)((exitOnly?196:236)*scale)};
    DWORD style=WS_POPUP|WS_CLIPCHILDREN;
    RECT owner; GetWindowRect(window,&owner);
    MONITORINFO info{sizeof(info)}; GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info);
    LONG w=bounds.right-bounds.left,h=bounds.bottom-bounds.top;
    LONG x=std::clamp((owner.left+owner.right-w)/2,info.rcWork.left,std::max(info.rcWork.left,info.rcWork.right-w));
    LONG y=std::clamp((owner.top+owner.bottom-h)/2,info.rcWork.top,std::max(info.rcWork.top,info.rcWork.bottom-h));
    closeDialog=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_LAYERED,L"LightRecClose",L"关闭全局录音",style,
        x,y,w,h,window,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!closeDialog) { note=L"无法打开关闭选项，请使用菜单"; return; }
    auto child=[&](const wchar_t* type,const wchar_t* label,DWORD flags,int id,int cx,int cy,int cw,int ch) {
        HWND item=CreateWindowExW(0,type,label,WS_CHILD|WS_VISIBLE|flags,
            (int)(cx*scale),(int)(cy*scale),(int)(cw*scale),(int)(ch*scale),
            closeDialog,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(item,WM_SETFONT,(WPARAM)closeFont,TRUE);
        SetWindowSubclass(item,closeControlProc,1,0);
    };
    child(L"STATIC",L"关闭全局录音",0,700,22,18,380,25);
    child(L"STATIC",exitOnly?L"停止并保存当前录音后退出？":L"收起到托盘，录音和转写会继续。",0,705,22,51,380,22);
    child(L"STATIC",exitOnly?L"保存失败时会保留窗口并显示错误。":
          busy()||previewRecording?L"退出程序会先停止并保存当前录音。":L"退出程序将结束录音工具。",
          0,704,22,76,380,30);
    child(L"BUTTON",L"记住我的选择",BS_AUTOCHECKBOX|WS_TABSTOP,703,22,111,220,24);
    child(L"BUTTON",L"最小化到托盘",BS_DEFPUSHBUTTON|WS_TABSTOP,701,22,157,145,31);
    child(L"BUTTON",exitOnly?L"保存并退出":L"退出程序",BS_PUSHBUTTON|WS_TABSTOP,702,178,157,112,31);
    child(L"BUTTON",L"取消",BS_PUSHBUTTON|WS_TABSTOP,IDCANCEL,302,157,100,31);
    child(L"BUTTON",L"取消关闭",BS_PUSHBUTTON|WS_TABSTOP,706,368,15,32,32);
    layoutClose(closeDialog);
    if(exitOnly) { ShowWindow(GetDlgItem(closeDialog,701),SW_HIDE); ShowWindow(GetDlgItem(closeDialog,703),SW_HIDE); }
    SetLayeredWindowAttributes(closeDialog,0,245,LWA_ALPHA);
    EnableWindow(window,FALSE);
    if(!diagnostic) {
        ShowWindow(closeDialog,SW_SHOW); SetForegroundWindow(closeDialog);
        SetFocus(GetDlgItem(closeDialog,exitOnly?702:701));
    }
}
bool renderClosePreview(const fs::path& path) {
    RECT client; GetClientRect(closeDialog,&client);
    HDC screen=GetDC(nullptr),memory=CreateCompatibleDC(screen);
    HBITMAP dib=CreateCompatibleBitmap(screen,client.right,client.bottom);
    HGDIOBJ previous=SelectObject(memory,dib);
    // Render our own hidden native children, avoiding non-client offsets and desktop capture.
    paintCloseBackground(closeDialog,memory);
    for(int id:{700,705,704,703,701,702,IDCANCEL,706}) {
        HWND child=GetDlgItem(closeDialog,id);
        if(!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)) continue;
        RECT bounds; GetWindowRect(child,&bounds);
        MapWindowPoints(nullptr,closeDialog,(POINT*)&bounds,2);
        int saved=SaveDC(memory);
        SetViewportOrgEx(memory,bounds.left,bounds.top,nullptr);
        IntersectClipRect(memory,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top);
        SendMessageW(child,WM_PRINT,(WPARAM)memory,PRF_CLIENT|PRF_ERASEBKGND);
        RestoreDC(memory,saved);
    }
    bool painted=false;
    COLORREF background=GetPixel(memory,0,0);
    for(int y=5;y<client.bottom&&!painted;y+=5) for(int x=5;x<client.right;x+=5)
        if(GetPixel(memory,x,y)!=background) { painted=true; break; }
    SelectObject(memory,previous);
    bool saved=false;
    {
        Bitmap bitmap(dib,nullptr),canvas(client.right,client.bottom,PixelFormat32bppPARGB);
        Graphics g(&canvas); g.Clear(Color(0,0,0,0));
        GraphicsPath outline; rounded(outline,0,0,(REAL)client.right,(REAL)client.bottom,20*closeScale);
        g.SetClip(&outline);
        ColorMatrix opacity={1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,245.f/255,0, 0,0,0,0,1};
        ImageAttributes attributes; attributes.SetColorMatrix(&opacity);
        g.DrawImage(&bitmap,Rect(0,0,client.right,client.bottom),0,0,client.right,client.bottom,UnitPixel,&attributes);
        g.Flush(); saved=painted&&savePng(canvas,path);
    }
    DeleteObject(dib); DeleteDC(memory); ReleaseDC(nullptr,screen);
    return saved;
}
bool dockWindowTest(const fs::path& target) {
    // Exercise only our own hidden HWND; no desktop input, devices, tray or global bindings.
    int checks=0; std::vector<int> failed;
    auto expect=[&](bool value) { ++checks; if(!value) failed.push_back(checks); };
    MONITORINFO info{sizeof(info)};
    expect(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info)!=FALSE);
    dockArea=info.rcWork;
    RECT area=dockArea;
    for(float scale:{1.f,1.25f,1.5f,2.f}) {
        dpi=scale;
        for(DockEdge edge:{DockEdge::Left,DockEdge::Right,DockEdge::Top,DockEdge::Bottom}) {
            dockEdge=edge; dockCollapsed=false;
            dockExpanded=dockBounds({area.left+100,area.top+100,area.left+100+(LONG)(W*dpi),
                area.top+100+(LONG)(H*dpi)},area,dpi,edge);
            RECT expected=dockExpanded; placeHud(expected); collapseDock();
            RECT actual; GetWindowRect(window,&actual);
            RECT tab=tabBounds(expected,area,dpi,edge);
            expect(dockCollapsed&&EqualRect(&actual,&tab));
            expandDock(); GetWindowRect(window,&actual);
            expect(!dockCollapsed&&EqualRect(&actual,&expected));
        }
    }
    dpi=GetDpiForWindow(window)/96.f;
    // A drop beyond the right edge must arm and complete hiding, not just clamp.
    RECT overshot{area.right-(LONG)(W*dpi)+40,area.top+100,
        area.right+40,area.top+100+(LONG)(H*dpi)};
    placeHud(overshot); finishDrag();
    expect(dockEdge==DockEdge::Right&&(dockCollapsed||(dockAnimating&&dockHiding)));
    advanceDockAnimation(dockAnimationStart+DockHideMs);
    RECT collapsedRect; GetWindowRect(window,&collapsedRect);
    expect(dockCollapsed&&collapsedRect.right==area.right&&
        collapsedRect.right-collapsedRect.left==(LONG)(24*dpi));
    RECT anchored=dockExpanded;
    expandDock();
    beginDockAnimation(true);
    diagnosticFrame=target/L"slide-out-mid.png";
    advanceDockAnimation(dockAnimationStart+DockHideMs/2);
    expect(dockAnimating&&!dockCollapsed&&dockReveal>0&&dockReveal<1);
    expect(fs::exists(target/L"slide-out-mid.png"));
    RECT during; GetWindowRect(window,&during);
    expect(EqualRect(&during,&anchored)); // Slide inside the same HWND; never jump monitors.
    advanceDockAnimation(dockAnimationStart+DockHideMs);
    expect(dockCollapsed&&!dockAnimating&&dockReveal==0);
    dockCollapsed=false;
    beginDockAnimation(false); placeHud(dockExpanded);
    diagnosticFrame=target/L"slide-in-mid.png";
    advanceDockAnimation(dockAnimationStart+DockShowMs/2);
    expect(fs::exists(target/L"slide-in-mid.png"));
    advanceDockAnimation(dockAnimationStart+DockShowMs);
    GetWindowRect(window,&during);
    expect(!dockCollapsed&&!dockAnimating&&dockReveal==1&&EqualRect(&during,&anchored));
    beginDockAnimation(true); advanceDockAnimation(dockAnimationStart+DockHideMs/2);
    expandDock(); GetWindowRect(window,&during);
    expect(!dockAnimating&&!dockCollapsed&&dockReveal==1&&EqualRect(&during,&anchored));
    KillTimer(window,2); dockEdge=DockEdge::None;
    RECT free{area.left+80,area.top+80,area.left+80+(LONG)(W*dpi),area.top+80+(LONG)(H*dpi)};
    placeHud(free); collapseDock();
    RECT unchanged; GetWindowRect(window,&unchanged);
    expect(!dockCollapsed&&EqualRect(&unchanged,&free));
    int hidden=0;
    expect(!hideWithTray([]{return false;},[&]{++hidden;})&&hidden==0);
    expect(hideWithTray([]{return true;},[&]{++hidden;})&&hidden==1);
    expect(!(GetWindowLongPtrW(window,GWL_STYLE)&WS_MINIMIZEBOX));
    LONG_PTR extended=GetWindowLongPtrW(window,GWL_EXSTYLE);
    expect((extended&WS_EX_TOOLWINDOW)&&!(extended&WS_EX_APPWINDOW)&&(extended&WS_EX_TOPMOST));
    previewRecording=true; showCloseChoice(); previewRecording=false;
    expect(closeDialog&&!IsWindowEnabled(window)&&
           IsDlgButtonChecked(closeDialog,703)==BST_UNCHECKED);
    expect(!(GetWindowLongPtrW(closeDialog,GWL_STYLE)&WS_CAPTION)&&
        (GetWindowLongPtrW(closeDialog,GWL_EXSTYLE)&WS_EX_LAYERED));
    BYTE alpha=0; DWORD layering=0; COLORREF key=0;
    expect(GetLayeredWindowAttributes(closeDialog,&key,&alpha,&layering)&&alpha==245);
    expect(SendMessageW(closeDialog,DM_GETDEFID,0,0)==MAKELONG(701,DC_HASDEFID));
    for(float scale:{1.f,1.25f,1.5f,2.f}) {
        closeScale=scale;
        SetWindowPos(closeDialog,nullptr,0,0,(int)(424*scale),(int)(236*scale),
                     SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        layoutClose(closeDialog);
        RECT client; GetClientRect(closeDialog,&client);
        for(int id:{700,705,704,703,701,702,IDCANCEL,706}) {
            RECT child; GetWindowRect(GetDlgItem(closeDialog,id),&child);
            MapWindowPoints(nullptr,closeDialog,(POINT*)&child,2);
            expect(child.left>=0&&child.top>=0&&child.right<=client.right&&child.bottom<=client.bottom);
        }
        expect(renderClosePreview(target/(L"close-choice-"+std::to_wstring((int)(scale*100))+L".png")));
    }
    HWND remember=GetDlgItem(closeDialog,703);
    SendMessageW(remember,BM_SETCHECK,BST_CHECKED,0);
    expect(IsDlgButtonChecked(closeDialog,703)==BST_CHECKED&&
        (GetWindowLongPtrW(remember,GWL_STYLE)&BS_TYPEMASK)==BS_AUTOCHECKBOX);
    SetWindowSubclass(remember,closeControlProc,1,2);
    SetWindowSubclass(GetDlgItem(closeDialog,701),closeControlProc,1,1);
    expect(renderClosePreview(target/L"close-choice.png"));
    closeIssue=true; SetDlgItemTextW(closeDialog,704,L"托盘暂不可用，请稍后重试，或选择退出程序。");
    expect(renderClosePreview(target/L"close-choice-error.png"));
    if(closeDialog) DestroyWindow(closeDialog);
    expect(!closeDialog&&IsWindowEnabled(window));
    showCloseChoice(true);
    expect(closeDialog&&closeExitOnly&&!(GetWindowLongPtrW(GetDlgItem(closeDialog,703),GWL_STYLE)&WS_VISIBLE)&&
        !(GetWindowLongPtrW(GetDlgItem(closeDialog,701),GWL_STYLE)&WS_VISIBLE));
    expect(SendMessageW(closeDialog,DM_GETDEFID,0,0)==MAKELONG(702,DC_HASDEFID));
    expect(renderClosePreview(target/L"exit-confirm.png"));
    closeProc(closeDialog,WM_COMMAND,IDCANCEL,0);
    expect(!closeDialog&&IsWindowEnabled(window));
    std::string list="[";
    for(size_t i=0;i<failed.size();++i) { if(i) list+=","; list+=std::to_string(failed[i]); }
    list+="]";
    bool saved=atomicText(target/L"native-dock-state-test.json",
        std::string("{\"passed\":")+(failed.empty()?"true":"false")+
        ",\"hiddenWindowOnly\":true,\"checks\":"+std::to_string(checks)+",\"failedChecks\":"+list+"}");
    return failed.empty()&&saved;
}
void command(int id) {
    struct Hold { Hold(){++interactionDepth; expandDock();} ~Hold(){--interactionDepth; outsideSince=0;} } hold;
    switch(id) {
    case Folder: chooseFolder(); break;
    case OpenFolder:
        ShellExecuteW(window,L"open",(session.empty()?root:session).c_str(),nullptr,nullptr,SW_SHOWNORMAL); break;
    case ShowText: showText(); break;
    case HideWindow: hideToTray(); break;
    case ShowWindowCmd: ShowWindow(window,SW_SHOWNORMAL); SetForegroundWindow(window); break;
    case EditHotkeys: editKeys(); break;
    case ToggleStartup: toggleStartup(); break;
    case ResetCloseAction:
        if(setting(L"close_action",L"ask")) { closeAction=CloseAction::Ask; note=L"点击 × 时将重新询问"; }
        else note=L"无法保存关闭选项，请重试";
        break;
    case ScopeGlobal:
        if(setting(L"program_only",L"0")) {
            programOnly=false; callHits=0; callLatched=false;
            note=busy()?L"已选择全局声音 · 下一次录音生效":L"已选择全局声音";
        }
        break;
    case ScopeProgram: chooseProgram(); break;
    case Retry: enabled=true; paused=false; flags(); startAsr(true); break;
    case CallOff: case CallAsk: case CallAuto:
        callMode=id-CallOff; callHits=0; callLatched=false;
        setting(L"call_mode",std::to_wstring(callMode)); break;
    case ExitApp:
        if(closeDialog) DestroyWindow(closeDialog);
        requestExit(); break;
    }
    redraw();
}
void menu() {
    struct Hold { Hold(){++interactionDepth; expandDock();} ~Hold(){--interactionDepth; outsideSince=0;} } hold;
    HMENU m=CreatePopupMenu(), calls=CreatePopupMenu(), scopes=CreatePopupMenu();
    AppendMenuW(scopes,MF_STRING|(!programOnly?MF_CHECKED:0),ScopeGlobal,L"全局声音");
    std::wstring selected=programOnly?L"指定程序："+fs::path(targetExe).filename().wstring()+L"…":L"指定程序…";
    AppendMenuW(scopes,MF_STRING|(programOnly?MF_CHECKED:0),ScopeProgram,selected.c_str());
    AppendMenuW(m,MF_POPUP,(UINT_PTR)scopes,L"监听范围");
    AppendMenuW(m,MF_STRING,Folder,L"选择录音保存文件夹…");
    AppendMenuW(m,MF_STRING,OpenFolder,L"打开录音文件夹");
    AppendMenuW(m,MF_STRING,ShowText,L"查看转写正文");
    AppendMenuW(m,MF_STRING,Retry,L"重试本次未完成转写");
    AppendMenuW(m,MF_STRING,EditHotkeys,L"快捷键设置…");
    AppendMenuW(m,MF_STRING|(closeAction==CloseAction::Ask?MF_GRAYED:0),ResetCloseAction,L"恢复关闭时询问");
    std::wstring startup;
    LONG startupState=lightrec::readStartup(startup);
    bool startupOn=startupState==ERROR_SUCCESS&&!startup.empty();
    AppendMenuW(m,MF_STRING|(startupOn?MF_CHECKED:0),ToggleStartup,L"开机自启动");
    AppendMenuW(m,MF_SEPARATOR,0,nullptr);
    AppendMenuW(calls,MF_STRING|(callMode==0?MF_CHECKED:0),CallOff,L"关闭");
    AppendMenuW(calls,MF_STRING|(callMode==1?MF_CHECKED:0),CallAsk,L"检测后询问");
    AppendMenuW(calls,MF_STRING|(callMode==2?MF_CHECKED:0),CallAuto,L"检测后自动录音");
    AppendMenuW(m,MF_POPUP,(UINT_PTR)calls,L"麦克风活动自动录音（实验）");
    AppendMenuW(m,MF_STRING,HideWindow,L"收起到托盘");
    AppendMenuW(m,MF_STRING,ShowWindowCmd,L"显示悬浮条");
    AppendMenuW(m,MF_SEPARATOR,0,nullptr);
    std::wstring shortcuts=keyName(recordKey)+L" 录音 · "+keyName(pauseKey)+L" 暂停转写";
    AppendMenuW(m,MF_STRING|MF_DISABLED,0,shortcuts.c_str());
    AppendMenuW(m,MF_STRING,ExitApp,L"退出");
    POINT p; GetCursorPos(&p); SetForegroundWindow(window);
    int id=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,window,nullptr);
    DestroyMenu(m); if(id) command(id);
}
LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(taskbarRestartMessage&&msg==taskbarRestartMessage&&!diagnostic) {
        trayAdded=false; lastTrayTip.clear(); installTray(true); return 0;
    }
    switch(msg) {
    case WM_TIMER: {
        if(wp==3) { advanceDockAnimation(GetTickCount64()); return 0; }
        if(wp==2) {
            if(dockEdge==DockEdge::None||!IsWindowVisible(hwnd)||IsIconic(hwnd)) return 0;
            if(dockAnimating) return 0;
            POINT cursor; GetCursorPos(&cursor); RECT rect; GetWindowRect(hwnd,&rect);
            bool inside=PtInRect(&rect,cursor)!=FALSE;
            bool held=dragPending||dragging||GetCapture()==hwnd||interactionDepth||!IsWindowEnabled(hwnd)||
                closeDialog||picker||hotkeyDialog;
            ULONGLONG now=GetTickCount64();
            if(dockCollapsed) {
                if(shouldExpandDock(inside,held,dockHoverArmed)) expandDock(true);
                return 0;
            }
            if(inside||held) outsideSince=0;
            else if(!outsideSince) outsideSince=now;
            if(shouldCollapse(dockCollapsed,held,inside,now,outsideSince)) collapseDock(true);
            return 0;
        }
        pollAsr();
        static auto previous=lightrec::CaptureState::Idle;
        auto current=recorder.state();
        if(current!=previous) {
            previous=current; flags();
            if(current==lightrec::CaptureState::Failed) { quitWhenSaved=false; note=recorder.error(); }
            if(current==lightrec::CaptureState::Saved) {
                note=recorder.error().empty()?L"已保存双轨与混音":recorder.error();
                if(quitWhenSaved) { DestroyWindow(hwnd); return 0; }
            }
        }
        static int ticks=0; ++ticks;
        if(ticks%4==0) { if(!diagnostic) { startAsr(); refreshTray(); } if(expanded) updateText(); }
        if(ticks%8==0) checkCall();
        redraw();
        if(benchTicks && ticks>=benchTicks) DestroyWindow(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if(dragPending) {
            POINT cursor; GetCursorPos(&cursor);
            if(!dragging&&(std::abs(cursor.x-dragStart.x)>=GetSystemMetrics(SM_CXDRAG)||
                           std::abs(cursor.y-dragStart.y)>=GetSystemMetrics(SM_CYDRAG))) {
                dragging=true; dockEdge=DockEdge::None; KillTimer(hwnd,2);
            }
            if(dragging) placeHud(draggedBounds(dragOrigin,dragStart,cursor));
            return 0;
        }
        if(dockCollapsed) { if(dockHoverArmed) expandDock(true); return 0; }
        if(dockAnimating) return 0;
        int h=hit(GET_X_LPARAM(lp)/dpi,GET_Y_LPARAM(lp)/dpi);
        if(h!=hover) { hover=h; redraw(); }
        TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,hwnd,0}; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: hover=-1; redraw(); return 0;
    case WM_SETCURSOR: if(hover>=0) { SetCursor(LoadCursorW(nullptr,IDC_HAND)); return TRUE; } break;
    case WM_LBUTTONDOWN: {
        bool fromDock=dockCollapsed||dockAnimating;
        if(fromDock) expandDock();
        int h=fromDock?-1:hit(GET_X_LPARAM(lp)/dpi,GET_Y_LPARAM(lp)/dpi);
        if(h<0) {
            GetCursorPos(&dragStart); GetWindowRect(hwnd,&dragOrigin);
            dragOffset={dragStart.x-dragOrigin.left,dragStart.y-dragOrigin.top};
            dragPending=true; dragging=false; SetCapture(hwnd); outsideSince=0; return 0;
        }
        if(diagnostic) return 0;
        if(h==6) { pressed=h; SetCapture(hwnd); redraw(); return 0; }
        if(h==0) recordToggle();
        if(h==1) { enabled=!enabled; if(enabled) paused=false; flags(); note.clear(); startAsr(); }
        if(h==2 && enabled) { paused=!paused; flags(); note.clear(); if(!paused) startAsr(); }
        if(h==3) menu();
        if(h==4) chooseFolder();
        redraw(); return 0;
    }
    case WM_LBUTTONUP: {
        if(dragPending) {
            bool moved=dragging; dragPending=dragging=false; ReleaseCapture();
            if(moved) { finishDrag(); rememberPosition(); }
            return 0;
        }
        int action=pressed;
        if(action!=6) break;
        int at=hit(GET_X_LPARAM(lp)/dpi,GET_Y_LPARAM(lp)/dpi);
        pressed=-1; ReleaseCapture();
        if(at==action) PostMessageW(hwnd,WM_CLOSE,0,0);
        redraw(); return 0;
    }
    case WM_CANCELMODE:
        if(GetCapture()==hwnd) ReleaseCapture();
        [[fallthrough]];
    case WM_CAPTURECHANGED: {
        bool moved=dragging; dragPending=dragging=false; pressed=-1;
        if(moved) { finishDrag(); rememberPosition(); }
        redraw(); return 0;
    }
    case WM_ENTERSIZEMOVE: expandDock(); ++interactionDepth; return 0;
    case WM_EXITSIZEMOVE: {
        if(interactionDepth) --interactionDepth;
        finishDrag();
        rememberPosition(); return 0;
    }
    case WM_SIZE: if(wp!=SIZE_MINIMIZED) redraw(); return 0;
    case WM_RBUTTONUP: if(!diagnostic) menu(); return 0;
    case WM_HOTKEY:
        if(hotkeyDialog||closeDialog) return 0;
        if(wp==1) recordToggle();
        if(wp==2 && enabled) { paused=!paused; flags(); if(!paused) startAsr(); }
        redraw(); return 0;
    case WM_TRAY: {
        UINT event=LOWORD(lp);
        if(event==WM_LBUTTONUP||event==NIN_SELECT||event==NIN_KEYSELECT) command(ShowWindowCmd);
        if(event==WM_RBUTTONUP||event==WM_CONTEXTMENU) menu();
        return 0;
    }
    case WM_DPICHANGED: {
        float oldDpi=dpi;
        dpi=HIWORD(wp)/96.f;
        RECT* r=(RECT*)lp;
        if(dragPending) {
            POINT cursor; GetCursorPos(&cursor);
            dragOffset.x=(LONG)std::lround(dragOffset.x*dpi/oldDpi);
            dragOffset.y=(LONG)std::lround(dragOffset.y*dpi/oldDpi);
            dragStart=cursor;
            dragOrigin={cursor.x-dragOffset.x,cursor.y-dragOffset.y,
                cursor.x-dragOffset.x+(LONG)(W*dpi),cursor.y-dragOffset.y+(LONG)(H*dpi)};
            placeHud(dragOrigin);
        } else {
            bool collapsed=dockCollapsed||(dockAnimating&&dockHiding);
            KillTimer(hwnd,3); dockAnimating=dockCollapsed=false; dockReveal=1;
            RECT rect{r->left,r->top,r->left+(LONG)(W*dpi),r->top+(LONG)(H*dpi)};
            if(dockEdge!=DockEdge::None) {
                MONITORINFO info{sizeof(info)};
                if(GetMonitorInfoW(MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST),&info)) dockArea=info.rcWork;
                rect=dockBounds(rect,dockArea,dpi,dockEdge);
            }
            dockExpanded=rect; placeHud(rect); if(collapsed) collapseDock();
        }
        redraw(); return 0;
    }
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE:
        if(dockEdge!=DockEdge::None) { expandDock(); finishDrag(); }
        return 0;
    case WM_CLOSE: {
        if(diagnostic) { DestroyWindow(hwnd); return 0; }
        ++interactionDepth; expandDock();
        if(closeAction==CloseAction::Tray) hideToTray();
        else if(closeAction==CloseAction::Exit) requestExit();
        else showCloseChoice();
        --interactionDepth; outsideSince=0; return 0;
    }
    case WM_DESTROY:
        if(!diagnostic) {
            enabled=false; paused=true; flags();
            if(hotkeyR) UnregisterHotKey(hwnd,1); if(hotkeyT) UnregisterHotKey(hwnd,2);
            if(trayAdded) Shell_NotifyIconW(NIM_DELETE,&tray);
            trayAdded=false;
            rememberPosition();
        }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t exe[32768]; GetModuleFileNameW(nullptr,exe,32768);
    currentExecutable=exe;
    project=fs::path(exe).parent_path().parent_path();
    config=project/L"native.ini";
    root=ini(L"folder",(project/L"recordings").wstring());
    targetExe=ini(L"target_exe");
    programOnly=!targetExe.empty() && ini(L"program_only",L"0")==L"1";
    preferredPid=GetPrivateProfileIntW(L"recorder",L"preferred_pid",0,config.c_str());
    session=ini(L"last_session"); activeRoot=session.empty()?root:session.parent_path();
    int argc=0; auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    std::wstring mode=argc>1?argv[1]:L"";
    diagnostic=mode==L"--render"||mode==L"--benchmark"||mode==L"--self-test"||mode==L"--config-test"||
        mode==L"--window-test"||mode==L"--export-icons"||mode==L"--shell-test"||mode==L"--dock-test";
    int result=0;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    GdiplusStartupInput input; GdiplusStartup(&gdipToken,&input,nullptr);
    if(mode==L"--export-icons") {
        result=argc>2&&lightrec::exportRecorderIcons(argv[2])?0:1;
    } else if(mode==L"--shell-test") {
        fs::path target=argc>2?fs::path(argv[2]):project/L"work";
        std::wstring startupIssue,trayIssue;
        bool startupOK=lightrec::startupSelfTest(startupIssue);
        bool trayOK=lightrec::traySelfTest(instance,trayIssue);
        bool iconsOK=true;
        for(int size:{16,20,24,32,48,64}) for(bool active:{false,true}) {
            HICON icon=lightrec::makeRecorderIcon(size,active);
            iconsOK=iconsOK&&icon; if(icon) DestroyIcon(icon);
        }
        result=startupOK&&trayOK&&iconsOK?0:1;
        atomicText(target/L"native-shell-test.json",std::string("{\"passed\":")+(result?"false":"true")+
            ",\"startupIsolatedRegistry\":"+(startupOK?"true":"false")+
            ",\"trayRegistrationAndRecreate\":"+(trayOK?"true":"false")+
            ",\"iconsAtSixSizes\":"+(iconsOK?"true":"false")+
            ",\"startupIssue\":"+escape(startupIssue)+",\"trayIssue\":"+escape(trayIssue)+"}");
    } else if(mode==L"--self-test") {
        std::wstring error;
        fs::path target=argc>2?fs::path(argv[2]):project/L"work";
        result=lightrec::audioSelfTest(target.wstring(),error)?0:1;
        atomicText(target/L"native-self-test.json","{\"passed\":"+std::string(result?"false":"true")+
                   ",\"error\":"+escape(error)+"}");
    } else if(mode==L"--window-test") {
        // Pure geometry/routing: no windows, devices or global hotkeys.
        int checks=0; bool ok=true;
        std::vector<int> failed;
        auto expect=[&](bool value) { ++checks; ok=ok&&value; if(!value) failed.push_back(checks); };
        RECT area{0,0,1920,1040};
        RECT left=dockBounds({10,300,458,426},area,1,DockEdge::Left);
        expect(left.left==-12&&left.right==436&&left.top==300);
        expect(edgeNear({10,300,458,426},area,1)==DockEdge::Left);
        RECT right=dockBounds({1465,300,1913,426},area,1,DockEdge::Right);
        expect(right.right==1932&&right.left==1484);
        expect(edgeNear({1465,300,1913,426},area,1)==DockEdge::Right);
        RECT top=dockBounds({500,10,948,136},area,1,DockEdge::Top);
        expect(top.top==-13&&top.bottom==113);
        RECT bottom=dockBounds({500,915,948,1041},area,1,DockEdge::Bottom);
        expect(bottom.bottom==1059&&bottom.top==933);
        expect(edgeNear({500,300,948,426},area,1)==DockEdge::None);
        RECT secondary{-1920,-100,0,980};
        RECT corner=dockBounds({-1914,-95,-1466,31},secondary,1,DockEdge::Left);
        expect(corner.left==-1932&&corner.top==-95);
        RECT scaled=dockBounds({10,10,906,262},area,2,DockEdge::Left);
        expect(scaled.left==-24&&scaled.top==10&&scaled.right-scaled.left==896);
        // Regression: moving PAST an edge used to clamp visually but never arm hiding.
        expect(edgeNear({-120,300,328,426},area,1)==DockEdge::Left);
        expect(edgeNear({1620,300,2068,426},area,1)==DockEdge::Right);
        expect(edgeNear({500,-100,948,26},area,1)==DockEdge::Top);
        expect(edgeNear({500,1000,948,1126},area,1)==DockEdge::Bottom);
        expect(edgeNear({-2050,200,-1602,326},secondary,1)==DockEdge::Left);
        expect(edgeNear({1400,300,2072,489},area,1.5f)==DockEdge::Right);
        // A collapsed window is a real, reachable tab wholly inside the selected work area.
        for(DockEdge edge:{DockEdge::Left,DockEdge::Right,DockEdge::Top,DockEdge::Bottom}) {
            for(float scale:{1.f,1.25f,1.5f,2.f}) {
                RECT full=dockBounds({-1850,200,-1850+(LONG)(W*scale),200+(LONG)(H*scale)},secondary,scale,edge);
                RECT tab=tabBounds(full,secondary,scale,edge);
                expect(tab.left>=secondary.left&&tab.right<=secondary.right&&
                       tab.top>=secondary.top&&tab.bottom<=secondary.bottom);
                expect(edge==DockEdge::Left?tab.left==secondary.left:
                    edge==DockEdge::Right?tab.right==secondary.right:
                    edge==DockEdge::Top?tab.top==secondary.top:tab.bottom==secondary.bottom);
                bool vertical=edge==DockEdge::Left||edge==DockEdge::Right;
                expect(tab.right-tab.left==(LONG)((vertical?24:48)*scale)&&
                       tab.bottom-tab.top==(LONG)((vertical?48:24)*scale));
            }
        }
        RECT movement=draggedBounds(left,{100,340},{280,365});
        expect(movement.left==168&&movement.top==325&&movement.right-movement.left==W);
        expect(edgeNear(movement,area,1)==DockEdge::None);
        // Reverse movement returns exactly to origin; no magnetic adjustment while captured.
        RECT back=draggedBounds(movement,{280,365},{100,340});
        expect(EqualRect(&back,&left));
        expect(!shouldCollapse(false,false,false,1649,1000)&&shouldCollapse(false,false,false,1650,1000));
        expect(!shouldCollapse(false,true,false,2000,1000)&&!shouldCollapse(false,false,true,2000,1000)&&
               !shouldCollapse(true,false,false,2000,1000)&&!shouldCollapse(false,false,false,2000,0));
        bool armed=false;
        expect(!shouldExpandDock(true,false,armed)&&!armed);
        expect(!shouldExpandDock(false,false,armed)&&armed&&shouldExpandDock(true,false,armed));
        expect(!shouldExpandDock(true,true,armed));
        expect(dockAnimationValue(0,1,true)==1&&dockAnimationValue(DockHideMs,1,true)==0);
        expect(dockAnimationValue(0,0,false)==0&&dockAnimationValue(DockShowMs,0,false)==1);
        expect(dockAnimationValue(40,1,true)>dockAnimationValue(80,1,true)&&
               dockAnimationValue(80,1,true)>dockAnimationValue(120,1,true));
        expect(std::abs(dockAnimationValue(0,.4f,false)-.4f)<.0001f&&
               dockAnimationValue(500,.4f,false)==1);
        expect(dockAnimationValue(80,1,true)>.85f&&dockAnimationValue(80,0,false)<.2f);
        expect(std::abs(dockAnimationValue(DockHideMs/2,1,true)-.5f)<.0001f);
        expect(dockAnimationValue(1,0,false)<.0001f&&dockAnimationValue(DockShowMs-1,0,false)>.9999f);
        expect(hit(382,28)==-1&&hit(410,28)==6);
        expect(hit(395,28)==-1&&hit(398,38)==6);
        expect(hit(395,58)==3&&hit(345,58)==2);
        expect(hit(410,40)==3&&hit(410,94)==4);
        expect(hit(396,28)==-1&&hit(425,28)==-1);
        fs::path target=argc>2?fs::path(argv[2]):project/L"work";
        config=target/L"native-hotkey-test.ini";
        expect(setting(L"close_action",L"ask")&&loadCloseAction()==CloseAction::Ask);
        expect(rememberCloseChoice(CloseAction::Tray,false)&&loadCloseAction()==CloseAction::Ask);
        expect(rememberCloseChoice(CloseAction::Tray,true)&&loadCloseAction()==CloseAction::Tray);
        expect(rememberCloseChoice(CloseAction::Ask,true)&&loadCloseAction()==CloseAction::Tray);
        expect(rememberCloseChoice(CloseAction::Exit,true)&&loadCloseAction()==CloseAction::Exit);
        expect(setting(L"close_action",L"ask")&&loadCloseAction()==CloseAction::Ask);
        expect(setting(L"close_action",L"invalid")&&loadCloseAction()==CloseAction::Ask);
        expect(exitStep(true,false)==ExitStep::Confirm&&exitStep(true,true)==ExitStep::Save);
        expect(exitStep(false,false)==ExitStep::Destroy&&exitStep(false,true)==ExitStep::Destroy);
        expect(sameKey(fromControl(controlKey({MOD_CONTROL|MOD_SHIFT,VK_F9})),{MOD_CONTROL|MOD_SHIFT,VK_F9}));
        expect(validKey({0,VK_F8})&&!validKey({0,'R'})&&!validKey({MOD_WIN,'R'}));
        int attempts=0,removals=0;
        auto accept=[&](int,Shortcut) { ++attempts; return true; };
        auto remove=[&](int) { ++removals; };
        std::wstring issue;
        expect(!replaceKeys({MOD_ALT,'R'},{MOD_ALT,'R'},issue,accept,remove)&&attempts==0&&removals==0);
        Shortcut nextRecord{MOD_CONTROL|MOD_ALT,VK_F7},nextPause{MOD_ALT,VK_F8};
        expect(replaceKeys(nextRecord,nextPause,issue,accept,remove)&&sameKey(recordKey,nextRecord)&&
               sameKey(pauseKey,nextPause)&&ini(L"record_key")==std::to_wstring(VK_F7));
        attempts=removals=0;
        Shortcut conflicting{MOD_CONTROL,VK_F6};
        auto conflict=[&](int,Shortcut key) { ++attempts; return !sameKey(key,conflicting); };
        expect(!replaceKeys(conflicting,{MOD_ALT,VK_F9},issue,conflict,remove)&&attempts==4&&removals==3&&
               sameKey(recordKey,nextRecord)&&sameKey(pauseKey,nextPause)&&hotkeyR&&hotkeyT);
        expect(ini(L"record_key")==std::to_wstring(VK_F7)&&ini(L"pause_key")==std::to_wstring(VK_F8));
        config=target/L"missing-config-directory"/L"cannot-write.ini";
        expect(!replaceKeys({MOD_CONTROL,VK_F10},{MOD_ALT,VK_F11},issue,accept,remove)&&
               sameKey(recordKey,nextRecord)&&sameKey(pauseKey,nextPause));
        CloseAction previousChoice=closeAction;
        expect(!rememberCloseChoice(CloseAction::Tray,true)&&closeAction==previousChoice);
        std::string failures="[";
        for(size_t i=0;i<failed.size();++i) { if(i) failures+=","; failures+=std::to_string(failed[i]); }
        failures+="]";
        atomicText(target/L"native-window-test.json",
            std::string("{\"passed\":")+(ok?"true":"false")+",\"checks\":"+std::to_string(checks)+
            ",\"failedChecks\":"+failures+"}");
        result=ok?0:1;
    } else if(mode==L"--config-test") {
        // Test the same INI reader/writer against an isolated file.
        if(argc<3) result=1;
        else {
            config=fs::path(argv[2])/L"native-config-test.ini";
            std::wstring selected=L"D:\\录音测试\\客户通话";
            bool ok=setting(L"folder",selected)&&ini(L"folder")==selected;
            ok=ok&&setting(L"target_exe",L"D:\\程序\\微信.exe")&&
                setting(L"program_only",L"1")&&ini(L"target_exe")==L"D:\\程序\\微信.exe"&&ini(L"program_only")==L"1";
            std::vector<Program> fake={{30,20,L"a.exe",L"D:\\a.exe"},
                {20,1,L"a.exe",L"D:\\a.exe"},{10,1,L"b.exe",L"D:\\b.exe"}};
            ok=ok&&rootPid(fake,L"D:\\a.exe",0)==20&&rootPid(fake,L"D:\\a.exe",30)==30&&
                rootPid(fake,L"D:\\missing.exe",0)==0;
            // Resolve portable ASR paths against isolated synthetic files only.
            project=fs::absolute(config.parent_path());
            auto fixture=project/L"asr-fixture";
            fs::create_directories(fixture/L"model");
            { std::ofstream(fixture/L"python.exe") << "synthetic";
              std::ofstream(fixture/L"model/model.bin") << "synthetic";
              std::ofstream(fixture/L"model/config.json") << "{}"; }
            bool asrPathsOK=setting(L"asr_python",L"asr-fixture/python.exe")&&
                setting(L"asr_model",L"asr-fixture/model");
            locateModel();
            asrPathsOK=asrPathsOK&&python==fixture/L"python.exe"&&model==fixture/L"model";
            asrPathsOK=asrPathsOK&&setting(L"asr_python",(fixture/L"python.exe").wstring())&&
                setting(L"asr_model",(fixture/L"model").wstring());
            locateModel();
            asrPathsOK=asrPathsOK&&python==fixture/L"python.exe"&&model==fixture/L"model";
            asrPathsOK=asrPathsOK&&setting(L"asr_model",L"asr-fixture/missing");
            locateModel();
            asrPathsOK=asrPathsOK&&model.empty();
            asrPathsOK=asrPathsOK&&setting(L"asr_python",L"")&&setting(L"asr_model",L"");
            locateModel();
            asrPathsOK=asrPathsOK&&python==project/L".venv-asr/Scripts/python.exe"&&model.empty();
            ok=ok&&asrPathsOK;
            atomicText(fs::path(argv[2])/L"native-config-test.json",
                       std::string("{\"passed\":")+(ok?"true":"false")+
                       ",\"portableAsrPaths\":"+(asrPathsOK?"true":"false")+
                       ",\"processLoopbackSupported\":"+
                       (lightrec::processLoopbackSupported()?"true":"false")+"}");
            result=ok?0:1;
        }
    } else if(mode==L"--render") {
        if(argc<3) result=1;
        else {
            previewRecording=argc>3 && std::wstring(argv[3])==L"recording";
            if(argc>4 && std::wstring(argv[4])==L"program") {
                programOnly=true; targetExe=L"Weixin.exe";
            }
            if(argc>5) hover=_wtoi(argv[5]);
            int tab=argc>6?(std::wstring(argv[6])==L"vertical"?1:
                std::wstring(argv[6])==L"horizontal"?2:
                std::wstring(argv[6])==L"right"?3:std::wstring(argv[6])==L"bottom"?4:0):0;
            result=render(argv[2],tab)?0:1;
        }
    } else {
        if(!diagnostic) {
            instanceMutex=CreateMutexW(nullptr,FALSE,L"Local\\GlobalRecorderNative20261105");
            if(!instanceMutex||GetLastError()==ERROR_ALREADY_EXISTS) {
                HWND old=FindWindowW(L"LightRecHUD",nullptr);
                if(old&&mode!=L"--background") { ShowWindow(old,SW_SHOWNORMAL); SetForegroundWindow(old); }
                if(instanceMutex) CloseHandle(instanceMutex);
                LocalFree(argv); GdiplusShutdown(gdipToken); CoUninitialize(); return 0;
            }
            locateModel();
            closeAction=loadCloseAction();
            Shortcut savedR{GetPrivateProfileIntW(L"recorder",L"record_modifiers",MOD_CONTROL|MOD_ALT,config.c_str()),
                GetPrivateProfileIntW(L"recorder",L"record_key",'R',config.c_str())};
            Shortcut savedT{GetPrivateProfileIntW(L"recorder",L"pause_modifiers",MOD_CONTROL|MOD_ALT,config.c_str()),
                GetPrivateProfileIntW(L"recorder",L"pause_key",'T',config.c_str())};
            if(validKey(savedR)&&validKey(savedT)&&!sameKey(savedR,savedT)) { recordKey=savedR; pauseKey=savedT; }
            {
                std::ifstream in(project/L"native-session-queue.txt",std::ios::binary);
                std::string line;
                while(std::getline(in,line))
                    if(!line.empty()) sessionQueue.emplace_back(wide(line));
                if(!session.empty()&&std::find(sessionQueue.begin(),sessionQueue.end(),session)==sessionQueue.end())
                    sessionQueue.push_back(session);
            }
            callMode=std::clamp(GetPrivateProfileIntW(L"recorder",L"call_mode",0,config.c_str()),0u,2u);
            flags();
        } else if(mode==L"--benchmark") benchTicks=argc>2?std::max(8,_wtoi(argv[2])*4):80;
        appLargeIcon=lightrec::makeRecorderIcon(64,false);
        appSmallIcon=lightrec::makeRecorderIcon(32,false);
        int trayPixels=GetSystemMetricsForDpi(SM_CXSMICON,GetDpiForSystem());
        trayIdleIcon=lightrec::makeRecorderIcon(trayPixels,false);
        trayRecordingIcon=lightrec::makeRecorderIcon(trayPixels,true);
        WNDCLASSW wc{}; wc.lpfnWndProc=proc; wc.hInstance=instance;
        const wchar_t* hudClass=diagnostic?L"LightRecHUDDiagnostic":L"LightRecHUD";
        wc.lpszClassName=hudClass; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        wc.hIcon=appLargeIcon;
        RegisterClassW(&wc);
        wc.lpfnWndProc=textProc; wc.lpszClassName=L"LightRecText";
        wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); RegisterClassW(&wc);
        wc.lpfnWndProc=pickerProc; wc.lpszClassName=L"LightRecProgram"; RegisterClassW(&wc);
        wc.lpfnWndProc=keysProc; wc.lpszClassName=L"LightRecKeys"; RegisterClassW(&wc);
        wc.lpfnWndProc=closeProc; wc.lpszClassName=L"LightRecClose";
        wc.hbrBackground=nullptr; RegisterClassW(&wc);
        dpi=GetDpiForSystem()/96.f; RECT area; SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0);
        int x=GetPrivateProfileIntW(L"recorder",L"x",area.right-(int)(W*dpi)-24,config.c_str());
        int y=GetPrivateProfileIntW(L"recorder",L"y",area.bottom-(int)(H*dpi)-24,config.c_str());
        // Restore a reachable position after monitor layout changes.
        RECT proposed{x,y,x+(int)(W*dpi),y+(int)(H*dpi)};
        if(!MonitorFromRect(&proposed,MONITOR_DEFAULTTONULL)) {
            x=area.right-(int)(W*dpi)-24; y=area.bottom-(int)(H*dpi)-24;
        }
        window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_TOOLWINDOW,hudClass,
            L"全局录音 · 原生轻量版",WS_POPUP|WS_SYSMENU,
            x,y,(int)(W*dpi),(int)(H*dpi),nullptr,nullptr,instance,nullptr);
        if(!window) result=1;
        else {
            SendMessageW(window,WM_SETICON,ICON_BIG,(LPARAM)appLargeIcon);
            SendMessageW(window,WM_SETICON,ICON_SMALL,(LPARAM)appSmallIcon);
            if(!diagnostic) {
                hotkeyR=registerBinding(1,recordKey);
                hotkeyT=registerBinding(2,pauseKey);
                if(!hotkeyR||!hotkeyT) note=L"快捷键被占用 · 可点击圆钮录音";
                taskbarRestartMessage=RegisterWindowMessageW(L"TaskbarCreated");
                if(!installTray(true)) note=L"托盘暂不可用 · 保留窗口并自动重试";
            }
            bool background=mode==L"--background"&&trayAdded;
            if(!diagnostic) {
                int savedEdge=GetPrivateProfileIntW(L"recorder",L"dock_edge",0,config.c_str());
                if(savedEdge>=1&&savedEdge<=4) {
                    dockEdge=(DockEdge)savedEdge;
                    MONITORINFO info{sizeof(info)};
                    if(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info)) {
                        dockArea=info.rcWork; RECT rect; GetWindowRect(window,&rect);
                        dockExpanded=dockBounds(rect,dockArea,dpi,dockEdge); placeHud(dockExpanded);
                        SetTimer(window,2,80,nullptr);
                    }
                }
            }
            if(mode==L"--dock-test") {
                fs::path target=argc>2?fs::path(argv[2]):project/L"work";
                result=dockWindowTest(target)?0:1; DestroyWindow(window);
            } else {
                redraw(); ShowWindow(window,diagnostic||background?SW_HIDE:SW_SHOWNOACTIVATE);
                SetTimer(window,1,250,nullptr);
            }
            MSG msg; while(GetMessageW(&msg,nullptr,0,0)>0) {
                if(picker && IsDialogMessageW(picker,&msg)) continue;
                if(hotkeyDialog && IsDialogMessageW(hotkeyDialog,&msg)) continue;
                if(closeDialog && IsDialogMessageW(closeDialog,&msg)) continue;
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
        }
    }
    if(asr) CloseHandle(asr);
    if(asrJob) CloseHandle(asrJob);
    if(instanceMutex) CloseHandle(instanceMutex);
    for(HICON icon:{appLargeIcon,appSmallIcon,trayIdleIcon,trayRecordingIcon}) if(icon) DestroyIcon(icon);
    LocalFree(argv); GdiplusShutdown(gdipToken); CoUninitialize();
    return result;
}
