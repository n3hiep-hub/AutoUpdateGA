// AutoUpdateClient — Game auto-updater with server-selection & AutoPlay popup
//
// UpdaterConfig.ini:
//   [ServerList]  1_Server=Name   1_host=http://...
//   [LastSel]     PicSel=0
//   [AutoPlay]    1_Text=...  1_Exe=...
//   [FontEdit]    FontDir=...  FontText=...
//   [GameConfig]  Graphics=3D  DisplayMode=fullscreen  Resolution=1024x768  ScreenshotPath=
//   [Updater]     GameExe=Game.exe
//   [AutoVLBS]    Exe=AutoVLBS.exe   Label=Mo AutoVLBS
//   [AutoPK]      Exe=AutoPK.exe     Label=Mo AutoPK

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <string>
#include <vector>
#include <sstream>
#include <thread>
#include <atomic>

#include "MD5Helper.h"
#include "HttpDownloader.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "ole32.lib")

static const wchar_t* kDefaultBaseURL = L"http://localhost/Update";
static const wchar_t* kDefaultGameExe = L"Game.exe";

// ---- Control IDs ---------------------------------------------------------- //
#define IDC_BTN_CHECK       101
#define IDC_BTN_GAME        102
#define IDC_BTN_CONFIG      103
#define IDC_BTN_AUTOVLBS    104   // NEW: Mo AutoVLBS
#define IDC_BTN_AUTOPK      105   // NEW: Mo AutoPK
#define IDC_SERVER_BASE     200
#define IDC_AUTO_BASE       300
#define IDC_FONT_BTN        350
#define IDC_THOAT_BTN       351
// Game-config dialog
#define IDC_CFG_R2D         401
#define IDC_CFG_R3D         402
#define IDC_CFG_RWIN        403
#define IDC_CFG_RFULL       404
#define IDC_CFG_R800        405
#define IDC_CFG_R1024       406
#define IDC_CFG_SCREDIT     407
#define IDC_CFG_BROWSE      408
#define IDC_CFG_SAVE        409
#define IDC_CFG_CANCEL      410

// ---- Window messages ------------------------------------------------------ //
#define WM_UPDATE_STATUS    (WM_USER + 1)
#define WM_UPDATE_PROGRESS  (WM_USER + 2)
#define WM_UPDATE_DONE      (WM_USER + 3)
#define WM_UPDATE_STARTED   (WM_USER + 4)

// ---- Server list ---------------------------------------------------------- //
struct ServerEntry { int id; std::wstring name, url; HWND hCheck = nullptr; };
static std::vector<ServerEntry> g_servers;
static int                      g_selectedServer = 0;

// ---- AutoPlay list -------------------------------------------------------- //
struct AutoPlayEntry { int id; std::wstring text, exe; };
static std::vector<AutoPlayEntry> g_autoPlay;
static std::wstring               g_fontDir;
static std::wstring               g_fontText;

// ---- AutoVLBS / AutoPK config -------------------------------------------- //
struct QuickLaunchEntry { std::wstring exe; std::wstring label; };
static QuickLaunchEntry g_autoVLBS;
static QuickLaunchEntry g_autoPK;

// ---- Game config ---------------------------------------------------------- //
struct GameConfig
{
    std::wstring graphics;
    std::wstring displayMode;
    std::wstring resolution;
    std::wstring screenshotPath;
    std::wstring packageDir;
    std::wstring packageDestFile;
};
static GameConfig g_gameConfig;

// ---- Dynamic layout ------------------------------------------------------- //
struct LayoutData
{
    static constexpr int W=480, ML=12, MR=40, BG=8;
    // Row 1: 3 buttons (Mo Auto | Vao Game | Cau hinh)
    // Row 2: 2 buttons (Mo AutoVLBS | Mo AutoPK) — full width split
    static constexpr int BW=140;   // width for row-1 buttons
    static constexpr int BW2=224;  // width for row-2 buttons  (2*224+8 = 456 fits in 480-12-12=456)
    int sepY=-1, statusY=0, statusH=20;
    int fileLY=0, fileLH=16, fileBY=0, fileBH=18;
    int ovrLY=0,  ovrLH=16,  ovrBY=0,  ovrBH=18;
    int btnY=0,   btnH=34,   barW=0,   pctX=0;
    int btn2Y=0;  // y for second button row
    static constexpr int pctW=30;
    int clientH=0;

    void Compute(int n)
    {
        barW=W-ML-MR; pctX=W-MR+4;
        int y=10;
        if(n>0){y+=n*24;sepY=y+4;y=sepY+2+10;}
        statusY=y; y+=statusH+8;
        fileLY=y; y+=fileLH+2; fileBY=y; y+=fileBH+8;
        ovrLY=y;  y+=ovrLH+2;  ovrBY=y;  y+=ovrBH+12;
        btnY=y;  y+=btnH+BG;
        btn2Y=y; clientH=y+btnH+10;
    }
};
static LayoutData g_lay;

// ---- UI handles ----------------------------------------------------------- //
static HWND g_hwnd=nullptr,g_hwndSep=nullptr,g_hwndStatus=nullptr;
static HWND g_hwndFileLabel=nullptr,g_hwndFileBar=nullptr,g_hwndFilePct=nullptr;
static HWND g_hwndOverallLabel=nullptr,g_hwndOverallBar=nullptr,g_hwndOverallPct=nullptr;
static HWND g_hwndBtnCheck=nullptr,g_hwndBtnGame=nullptr,g_hwndBtnConfig=nullptr;
static HWND g_hwndBtnAutoVLBS=nullptr, g_hwndBtnAutoPK=nullptr;  // NEW

// ---- GDI ------------------------------------------------------------------ //
static HBRUSH g_hBrushBg=nullptr,g_hBrushBtn=nullptr,g_hBrushEdit=nullptr;
static HFONT  g_hFont=nullptr,g_hFontSmall=nullptr;

// ---- App state ------------------------------------------------------------ //
static std::atomic<bool> g_isUpdating{false};

// ---- Exe info ------------------------------------------------------------- //
struct ExeInfo { std::wstring path,dir,filename,stem; };
static ExeInfo g_exe;
static void InitExeInfo()
{
    wchar_t buf[MAX_PATH]={};
    GetModuleFileNameW(nullptr,buf,MAX_PATH);
    g_exe.path=buf;
    auto sl=g_exe.path.rfind(L'\\');
    if(sl!=std::wstring::npos){g_exe.dir=g_exe.path.substr(0,sl);g_exe.filename=g_exe.path.substr(sl+1);}
    else{g_exe.dir=L".";g_exe.filename=g_exe.path;}
    auto dt=g_exe.filename.rfind(L'.');
    g_exe.stem=(dt!=std::wstring::npos)?g_exe.filename.substr(0,dt):g_exe.filename;
}

// ---- Config --------------------------------------------------------------- //
struct AppConfig { std::wstring gameExe; };
static AppConfig    g_config;
static std::wstring g_configPath;

// Helper: launch an exe relative to our directory
static void LaunchExe(const std::wstring& exeName, HWND hParent)
{
    if(exeName.empty()){
        MessageBoxW(hParent,L"Chua cau hinh exe trong UpdaterConfig.ini.",L"Thong bao",MB_ICONINFORMATION);
        return;
    }
    std::wstring ep = g_exe.dir + L"\\" + exeName;
    for(auto& c : ep) if(c==L'/') c=L'\\';
    if(GetFileAttributesW(ep.c_str())==INVALID_FILE_ATTRIBUTES){
        MessageBoxW(hParent,(L"Khong tim thay:\n"+ep).c_str(),L"Khong tim thay",MB_ICONWARNING);
        return;
    }
    ShellExecuteW(nullptr,L"open",ep.c_str(),nullptr,g_exe.dir.c_str(),SW_SHOWNORMAL);
}

static void LoadConfig()
{
    wchar_t buf[1024]={};

    // [ServerList]
    g_servers.clear();
    for(int i=1;i<=32;++i){
        GetPrivateProfileStringW(L"ServerList",(std::to_wstring(i)+L"_Server").c_str(),L"",buf,_countof(buf),g_configPath.c_str());
        if(!buf[0])break;
        ServerEntry se;se.id=i;se.name=buf;
        GetPrivateProfileStringW(L"ServerList",(std::to_wstring(i)+L"_host").c_str(),kDefaultBaseURL,buf,_countof(buf),g_configPath.c_str());
        se.url=buf;g_servers.push_back(se);
    }
    // [LastSel]
    GetPrivateProfileStringW(L"LastSel",L"PicSel",L"0",buf,_countof(buf),g_configPath.c_str());
    g_selectedServer=_wtoi(buf);
    // [AutoPlay]
    g_autoPlay.clear();
    for(int i=1;i<=32;++i){
        GetPrivateProfileStringW(L"AutoPlay",(std::to_wstring(i)+L"_Text").c_str(),L"",buf,_countof(buf),g_configPath.c_str());
        if(!buf[0])break;
        AutoPlayEntry e;e.id=i;e.text=buf;
        GetPrivateProfileStringW(L"AutoPlay",(std::to_wstring(i)+L"_Exe").c_str(),L"",buf,_countof(buf),g_configPath.c_str());
        e.exe=buf;g_autoPlay.push_back(e);
    }
    // [FontEdit]
    GetPrivateProfileStringW(L"FontEdit",L"FontDir",L"",buf,_countof(buf),g_configPath.c_str());g_fontDir=buf;
    GetPrivateProfileStringW(L"FontEdit",L"FontText",L"Sua loi Font chu",buf,_countof(buf),g_configPath.c_str());g_fontText=buf;
    // [GameConfig]
    GetPrivateProfileStringW(L"GameConfig",L"Graphics",L"3D",buf,_countof(buf),g_configPath.c_str());g_gameConfig.graphics=buf;
    GetPrivateProfileStringW(L"GameConfig",L"DisplayMode",L"fullscreen",buf,_countof(buf),g_configPath.c_str());g_gameConfig.displayMode=buf;
    GetPrivateProfileStringW(L"GameConfig",L"Resolution",L"1024x768",buf,_countof(buf),g_configPath.c_str());g_gameConfig.resolution=buf;
    GetPrivateProfileStringW(L"GameConfig",L"ScreenshotPath",L"",buf,_countof(buf),g_configPath.c_str());g_gameConfig.screenshotPath=buf;
    GetPrivateProfileStringW(L"GameConfig",L"PackageDir",L"Package",buf,_countof(buf),g_configPath.c_str());g_gameConfig.packageDir=buf;
    GetPrivateProfileStringW(L"GameConfig",L"PackageDestFile",L"package.ini",buf,_countof(buf),g_configPath.c_str());g_gameConfig.packageDestFile=buf;
    // [Updater]
    GetPrivateProfileStringW(L"Updater",L"GameExe",kDefaultGameExe,buf,_countof(buf),g_configPath.c_str());g_config.gameExe=buf;

    // [AutoVLBS] — NEW
    GetPrivateProfileStringW(L"AutoVLBS",L"Exe",L"",buf,_countof(buf),g_configPath.c_str());
    g_autoVLBS.exe=buf;
    GetPrivateProfileStringW(L"AutoVLBS",L"Label",L"Mo AutoVLBS",buf,_countof(buf),g_configPath.c_str());
    g_autoVLBS.label=buf;

    // [AutoPK] — NEW
    GetPrivateProfileStringW(L"AutoPK",L"Exe",L"",buf,_countof(buf),g_configPath.c_str());
    g_autoPK.exe=buf;
    GetPrivateProfileStringW(L"AutoPK",L"Label",L"Mo AutoPK",buf,_countof(buf),g_configPath.c_str());
    g_autoPK.label=buf;
}

static void SaveSelectedServer(){WritePrivateProfileStringW(L"LastSel",L"PicSel",std::to_wstring(g_selectedServer).c_str(),g_configPath.c_str());}
static void SaveGameConfig()
{
    WritePrivateProfileStringW(L"GameConfig",L"Graphics",g_gameConfig.graphics.c_str(),g_configPath.c_str());
    WritePrivateProfileStringW(L"GameConfig",L"DisplayMode",g_gameConfig.displayMode.c_str(),g_configPath.c_str());
    WritePrivateProfileStringW(L"GameConfig",L"Resolution",g_gameConfig.resolution.c_str(),g_configPath.c_str());
    WritePrivateProfileStringW(L"GameConfig",L"ScreenshotPath",g_gameConfig.screenshotPath.c_str(),g_configPath.c_str());
}
static std::wstring GetServerURL(){
    for(const auto&s:g_servers)if(s.id==g_selectedServer)return s.url;
    if(g_servers.empty())return kDefaultBaseURL;
    return{};
}

// ---- INI parser ----------------------------------------------------------- //
struct FileEntry{std::string relativePath,expectedMD5;};
static std::vector<FileEntry> ParseIni(const std::string&text,std::wstring&outBase,const std::wstring&fallback)
{
    outBase=fallback;std::vector<FileEntry>entries;std::istringstream ss(text);std::string line;bool inF=false;
    while(std::getline(ss,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.empty()||line[0]==';')continue;
        if(line[0]=='['){inF=(line.substr(1,line.find(']')-1)=="Files");continue;}
        auto eq=line.find('=');if(eq==std::string::npos)continue;
        std::string k=line.substr(0,eq),v=line.substr(eq+1);
        if(!inF){if(k=="BaseURL"&&!v.empty())outBase={v.begin(),v.end()};continue;}
        FileEntry fe;fe.relativePath=k;fe.expectedMD5=v;
        for(auto&c:fe.expectedMD5)c=(char)tolower((unsigned char)c);
        entries.push_back(std::move(fe));
    }
    return entries;
}
static std::wstring ToLocal(const std::string&r){std::wstring w(r.begin(),r.end());for(auto&c:w)if(c==L'/')c=L'\\';return w;}
static std::wstring ToURL(const std::string&r){std::wstring w(r.begin(),r.end());for(auto&c:w)if(c==L'\\')c=L'/';return w;}
static std::wstring MakeURL(const std::wstring&base,const std::string&rel){std::wstring u=base;if(!u.empty()&&u.back()==L'/')u.pop_back();u+=L'/';u+=ToURL(rel);return u;}

// ---- UI helpers ----------------------------------------------------------- //
static void PostStatus(const std::wstring&msg){PostMessageW(g_hwnd,WM_UPDATE_STATUS,0,(LPARAM)(new std::wstring(msg)));}
static void PostProgress(int f,int o){PostMessageW(g_hwnd,WM_UPDATE_PROGRESS,(WPARAM)f,(LPARAM)o);}
static void SetBar(HWND hBar,HWND hPct,int val,int total){
    SendMessageW(hBar,PBM_SETRANGE32,0,total);SendMessageW(hBar,PBM_SETPOS,val,0);
    if(total>0){wchar_t b[16];swprintf_s(b,L"%d%%",(int)(100LL*val/total));SetWindowTextW(hPct,b);}
}

// ---- Self-update ---------------------------------------------------------- //
static bool ApplySelfUpdate(const std::wstring&pending){
    auto bak=g_exe.path+L".old";DeleteFileW(bak.c_str());
    if(!MoveFileW(g_exe.path.c_str(),bak.c_str()))return false;
    if(!MoveFileW(pending.c_str(),g_exe.path.c_str())){MoveFileW(bak.c_str(),g_exe.path.c_str());DeleteFileW(pending.c_str());return false;}
    return true;
}

// ==========================================================================
//  UPDATE WORKER
// ==========================================================================
static void RunUpdate()
{
    g_isUpdating=true;PostMessageW(g_hwnd,WM_UPDATE_STARTED,0,0);
    auto srv=GetServerURL();
    if(srv.empty()){PostStatus(L"Vui long chon server truoc.");g_isUpdating=false;PostMessageW(g_hwnd,WM_UPDATE_DONE,1,0);return;}
    PostStatus(L"Dang tai danh sach cap nhat...");PostProgress(0,0);
    auto dl=AutoUpdate::DownloadToMemory(MakeURL(srv,"AutoUpdate.ini"));
    if(!dl.success||dl.content.empty()){MessageBoxW(nullptr,(L"Khong tai duoc AutoUpdate.ini tu:\n"+srv).c_str(),L"Loi",MB_ICONERROR);g_isUpdating=false;PostMessageW(g_hwnd,WM_UPDATE_DONE,0,0);return;}
    std::wstring base;auto entries=ParseIni(dl.content,base,srv);
    PostStatus(L"Dang kiem tra file...");
    std::vector<const FileEntry*>toU;const FileEntry*selfE=nullptr;
    for(const auto&e:entries){
        auto rel=ToLocal(e.relativePath);
        if(_wcsicmp(rel.c_str(),g_exe.filename.c_str())==0){if(_stricmp(AutoUpdate::ComputeFileMD5(g_exe.path).c_str(),e.expectedMD5.c_str())!=0)selfE=&e;continue;}
        auto lp=g_exe.dir+L"\\"+rel;bool need=(GetFileAttributesW(lp.c_str())==INVALID_FILE_ATTRIBUTES);
        if(!need)need=(_stricmp(AutoUpdate::ComputeFileMD5(lp).c_str(),e.expectedMD5.c_str())!=0);if(need)toU.push_back(&e);
    }
    int total=(int)toU.size()+(selfE?1:0);
    if(total==0){PostStatus(L"Game da o phien ban moi nhat!");PostProgress(1000,1000);g_isUpdating=false;PostMessageW(g_hwnd,WM_UPDATE_DONE,1,0);return;}
    DWORD lastTick=0;
    for(int i=0;i<(int)toU.size();++i){
        const FileEntry*e=toU[i];std::wstring rw(e->relativePath.begin(),e->relativePath.end());
        PostStatus(L"Dang tai ("+std::to_wstring(i+1)+L"/"+std::to_wstring(total)+L")  "+rw);
        auto dest=g_exe.dir+L"\\"+ToLocal(e->relativePath);
        auto cb=[&](DWORD64 r,DWORD64 t){if(GetTickCount()-lastTick<50)return;lastTick=GetTickCount();int fp=t?(int)(r*1000ull/t):500,op=(int)(((DWORD64)i*1000ull+fp)/(DWORD64)total);PostProgress(fp,op);};
        bool ok=AutoUpdate::DownloadToFile(MakeURL(base,e->relativePath),dest,cb);
        if(!ok){Sleep(1500);ok=AutoUpdate::DownloadToFile(MakeURL(base,e->relativePath),dest,nullptr);}
        if(!ok){MessageBoxW(nullptr,(L"Khong tai duoc:\n"+rw).c_str(),L"Loi",MB_ICONERROR);g_isUpdating=false;PostMessageW(g_hwnd,WM_UPDATE_DONE,0,0);return;}
        PostProgress(1000,(int)((DWORD64)(i+1)*1000ull/(DWORD64)total));
    }
    if(selfE){
        int si=(int)toU.size();PostStatus(L"Dang tai ban moi AutoUpdateClient...");
        auto tmp=g_exe.dir+L"\\"+g_exe.stem+L"_pending.bin";DeleteFileW(tmp.c_str());
        auto cb=[&](DWORD64 r,DWORD64 t){if(GetTickCount()-lastTick<50)return;lastTick=GetTickCount();int fp=t?(int)(r*1000ull/t):500,op=(int)(((DWORD64)si*1000ull+fp)/(DWORD64)total);PostProgress(fp,op);};
        bool ok=AutoUpdate::DownloadToFile(MakeURL(base,selfE->relativePath),tmp,cb);
        if(!ok){Sleep(1500);ok=AutoUpdate::DownloadToFile(MakeURL(base,selfE->relativePath),tmp,nullptr);}
        if(!ok){MessageBoxW(nullptr,L"Khong tai duoc ban moi AutoUpdateClient.",L"Loi",MB_ICONWARNING);g_isUpdating=false;PostMessageW(g_hwnd,WM_UPDATE_DONE,1,0);return;}
        PostProgress(1000,1000);
        if(ApplySelfUpdate(tmp)){ShellExecuteW(nullptr,L"open",g_exe.path.c_str(),nullptr,g_exe.dir.c_str(),SW_SHOWNORMAL);PostMessageW(g_hwnd,WM_CLOSE,0,0);}
        else{DeleteFileW(tmp.c_str());MessageBoxW(nullptr,L"Khong the thay the AutoUpdateClient.exe.",L"Loi",MB_ICONWARNING);g_isUpdating=false;PostMessageW(g_hwnd,WM_UPDATE_DONE,1,0);}
        return;
    }
    PostStatus(L"Cap nhat hoan tat!");PostProgress(1000,1000);g_isUpdating=false;PostMessageW(g_hwnd,WM_UPDATE_DONE,1,0);
}

// ==========================================================================
//  AUTOPLAY POPUP
// ==========================================================================
static const COLORREF kPopBg=RGB(75,10,10),kTitleBg=RGB(100,15,15);
static const COLORREF kBtnFace=RGB(120,22,22),kBtnBord=RGB(160,45,45);
static const COLORREF kThoatFace=RGB(205,75,20),kThoatBord=RGB(235,100,45);
static HFONT g_hPopTitle=nullptr,g_hPopBtn=nullptr;

static void DrawPopupBtn(DRAWITEMSTRUCT*dis,COLORREF face,COLORREF border)
{
    HDC dc=dis->hDC;RECT r=dis->rcItem;
    HBRUSH hbr=CreateSolidBrush(face);FillRect(dc,&r,hbr);DeleteObject(hbr);
    HPEN hp=CreatePen(PS_SOLID,2,border);HPEN op=(HPEN)SelectObject(dc,hp);
    SelectObject(dc,GetStockObject(NULL_BRUSH));Rectangle(dc,r.left,r.top,r.right-1,r.bottom-1);
    SelectObject(dc,op);DeleteObject(hp);
    wchar_t txt[256]={};GetWindowTextW(dis->hwndItem,txt,256);
    HFONT of=(HFONT)SelectObject(dc,g_hPopBtn);
    SetTextColor(dc,RGB(255,230,200));SetBkMode(dc,TRANSPARENT);
    RECT tr=r;tr.top+=2;DrawTextW(dc,txt,-1,&tr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc,of);
}

static LRESULT CALLBACK AutoPlayProc(HWND hwnd,UINT msg,WPARAM wParam,LPARAM lParam)
{
    static bool*s_done=nullptr;
    switch(msg){
    case WM_CREATE:{
        s_done=(bool*)((CREATESTRUCTW*)lParam)->lpCreateParams;
        if(!g_hPopTitle)g_hPopTitle=CreateFontW(20,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        if(!g_hPopBtn)  g_hPopBtn  =CreateFontW(16,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        HINSTANCE hi=GetModuleHandleW(nullptr);constexpr int BX=12,BW=396,BH=52,BG=8;int y=62;
        for(const auto&e:g_autoPlay){HWND h=CreateWindowExW(0,L"BUTTON",e.text.c_str(),WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,BX,y,BW,BH,hwnd,(HMENU)(INT_PTR)(IDC_AUTO_BASE+e.id),hi,nullptr);SendMessageW(h,WM_SETFONT,(WPARAM)g_hPopBtn,TRUE);y+=BH+BG;}
        if(!g_fontDir.empty()){HWND h=CreateWindowExW(0,L"BUTTON",g_fontText.c_str(),WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,BX,y,BW,BH,hwnd,(HMENU)IDC_FONT_BTN,hi,nullptr);SendMessageW(h,WM_SETFONT,(WPARAM)g_hPopBtn,TRUE);y+=BH+BG;}
        {HWND h=CreateWindowExW(0,L"BUTTON",L"Thoat",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,BX,y,BW,44,hwnd,(HMENU)IDC_THOAT_BTN,hi,nullptr);SendMessageW(h,WM_SETFONT,(WPARAM)g_hPopBtn,TRUE);}
        return 0;
    }
    case WM_DRAWITEM:{auto*dis=(DRAWITEMSTRUCT*)lParam;DrawPopupBtn(dis,dis->CtlID==IDC_THOAT_BTN?kThoatFace:kBtnFace,dis->CtlID==IDC_THOAT_BTN?kThoatBord:kBtnBord);return TRUE;}
    case WM_COMMAND:{
        int id=LOWORD(wParam);
        if(id==IDC_THOAT_BTN){DestroyWindow(hwnd);return 0;}
        if(id==IDC_FONT_BTN&&!g_fontDir.empty()){std::wstring p=g_exe.dir+L"\\"+g_fontDir;for(auto&c:p)if(c==L'/')c=L'\\';ShellExecuteW(nullptr,L"open",p.c_str(),nullptr,g_exe.dir.c_str(),SW_SHOWNORMAL);return 0;}
        if(id>IDC_AUTO_BASE&&id<=IDC_AUTO_BASE+32){int idx=id-IDC_AUTO_BASE;for(const auto&e:g_autoPlay){if(e.id==idx){if(!e.exe.empty()){std::wstring ep=g_exe.dir+L"\\"+e.exe;for(auto&c:ep)if(c==L'/')c=L'\\';ShellExecuteW(nullptr,L"open",ep.c_str(),nullptr,g_exe.dir.c_str(),SW_SHOWNORMAL);}else{MessageBoxW(hwnd,(L"Chua cau hinh Exe cho:\n"+e.text).c_str(),L"Thong bao",MB_ICONINFORMATION);}break;}}return 0;}
        return 0;
    }
    case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);RECT wr;GetClientRect(hwnd,&wr);RECT tr={0,0,wr.right,55};HBRUSH hbr=CreateSolidBrush(kTitleBg);FillRect(dc,&tr,hbr);DeleteObject(hbr);HPEN hp=CreatePen(PS_SOLID,2,kBtnBord);HPEN op=(HPEN)SelectObject(dc,hp);MoveToEx(dc,0,54,nullptr);LineTo(dc,wr.right,54);SelectObject(dc,op);DeleteObject(hp);HFONT of=(HFONT)SelectObject(dc,g_hPopTitle);SetTextColor(dc,RGB(255,220,170));SetBkMode(dc,TRANSPARENT);DrawTextW(dc,L"LUA CHON AUTO",-1,&tr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,of);EndPaint(hwnd,&ps);return 0;}
    case WM_ERASEBKGND:{RECT r;GetClientRect(hwnd,&r);HBRUSH hbr=CreateSolidBrush(kPopBg);FillRect((HDC)wParam,&r,hbr);DeleteObject(hbr);return 1;}
    case WM_DESTROY:if(s_done)*s_done=true;return 0;
    }
    return DefWindowProcW(hwnd,msg,wParam,lParam);
}

static void ShowAutoPlayDialog(HWND hParent)
{
    static bool reg=false;
    if(!reg){WNDCLASSEXW wc={};wc.cbSize=sizeof(wc);wc.lpfnWndProc=AutoPlayProc;wc.hInstance=GetModuleHandleW(nullptr);wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);wc.lpszClassName=L"AutoPlayDlg";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassExW(&wc);reg=true;}
    constexpr int BH=52,BG=8;bool done=false;
    int H=55+(int)g_autoPlay.size()*(BH+BG)+(!g_fontDir.empty()?(BH+BG):0)+44+BG+16,W=420;
    RECT pr;GetWindowRect(hParent,&pr);
    HWND hDlg=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_TOPMOST,L"AutoPlayDlg",L"",WS_POPUP|WS_BORDER,
        pr.left+(pr.right-pr.left-W)/2,pr.top+(pr.bottom-pr.top-H)/2,W,H,hParent,nullptr,GetModuleHandleW(nullptr),&done);
    EnableWindow(hParent,FALSE);ShowWindow(hDlg,SW_SHOW);
    MSG msg;while(!done){if(GetMessageW(&msg,nullptr,0,0)==0){PostQuitMessage((int)msg.wParam);break;}if(!IsDialogMessageW(hDlg,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    EnableWindow(hParent,TRUE);SetForegroundWindow(hParent);
}

// ==========================================================================
//  GAME CONFIG DIALOG
// ==========================================================================
static const COLORREF kCfgBg      = RGB(75,  45, 20);
static const COLORREF kCfgTitleBg = RGB(155, 30, 30);
static const COLORREF kCfgGrpBg   = RGB(90,  55, 28);
static const COLORREF kCfgGrpBrd  = RGB(140, 85, 40);
static const COLORREF kCfgBtnFace = RGB(175, 35, 35);
static const COLORREF kCfgBtnBrd  = RGB(210, 65, 55);
static const COLORREF kCfgEditBg  = RGB(50,  30, 10);
static const COLORREF kCfgText    = RGB(240, 210,170);
static const COLORREF kCfgBtnText = RGB(255, 255,255);

static const wchar_t* kResStr[] = { L"800x600", L"1024x768" };

static void ApplyResolutionPackage(int resIdx)
{
    const std::wstring resName = kResStr[resIdx];
    std::wstring src = g_exe.dir + L"\\" + g_gameConfig.packageDir + L"\\" + resName + L".ini";
    std::wstring dst = g_exe.dir + L"\\" + g_gameConfig.packageDestFile;
    for (auto& c : src) if (c == L'/') c = L'\\';
    for (auto& c : dst) if (c == L'/') c = L'\\';
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES){
        MessageBoxW(nullptr,(L"Khong tim thay file:\n" + src).c_str(),L"Package", MB_ICONWARNING | MB_OK);
        return;
    }
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)){
        MessageBoxW(nullptr,(L"Khong the chep file:\n" + src + L"\n->\n" + dst).c_str(),L"Package", MB_ICONERROR | MB_OK);
    }
}

struct GameCfgState
{
    int  selGraphics  = 1;
    int  selDisplay   = 1;
    int  selRes       = 1;
    std::wstring screenshotPath;
    HWND hR[3][2]     = {};
    HWND hScreenEdit  = nullptr;
    HFONT hTitleFont  = nullptr;
    HFONT hBtnFont    = nullptr;
    HFONT hLblFont    = nullptr;
    HBRUSH hBrushBg   = nullptr;
    HBRUSH hBrushEdit = nullptr;
    HBRUSH hBrushGrp  = nullptr;
    bool confirmed = false;
    bool done      = false;
};

static void DrawCfgRadio(DRAWITEMSTRUCT* dis, GameCfgState* st)
{
    HDC dc = dis->hDC; RECT r = dis->rcItem;
    HBRUSH hbg = CreateSolidBrush(kCfgGrpBg); FillRect(dc, &r, hbg); DeleteObject(hbg);
    bool checked = false;
    for(int g=0;g<3;g++) for(int o=0;o<2;o++) if(dis->hwndItem==st->hR[g][o]) {
        if(g==0) checked=(st->selGraphics==o);
        else if(g==1) checked=(st->selDisplay==o);
        else          checked=(st->selRes==o);
    }
    int cy=(r.top+r.bottom)/2, cx=r.left+18;
    HPEN hp = CreatePen(PS_SOLID, 2, RGB(220,200,175));
    HPEN op = (HPEN)SelectObject(dc, hp);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx-10, cy-10, cx+10, cy+10);
    SelectObject(dc, op); DeleteObject(hp);
    if(checked) {
        HBRUSH hfill = CreateSolidBrush(RGB(255,255,255));
        HPEN   hp2   = CreatePen(PS_SOLID,1,RGB(255,255,255));
        SelectObject(dc, hfill); SelectObject(dc, hp2);
        Ellipse(dc, cx-5, cy-5, cx+5, cy+5);
        DeleteObject(hfill); DeleteObject(hp2);
    }
    wchar_t txt[128]={}; GetWindowTextW(dis->hwndItem, txt, 128);
    HFONT of = (HFONT)SelectObject(dc, st->hLblFont);
    RECT tr = {cx+14, r.top, r.right, r.bottom};
    SetTextColor(dc, kCfgText); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, txt, -1, &tr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc, of);
}

static void DrawCfgBtn(DRAWITEMSTRUCT* dis, GameCfgState* st, COLORREF face=kCfgBtnFace)
{
    HDC dc=dis->hDC; RECT r=dis->rcItem;
    HBRUSH hbr=CreateSolidBrush(face); FillRect(dc,&r,hbr); DeleteObject(hbr);
    HPEN hp=CreatePen(PS_SOLID,2,kCfgBtnBrd); HPEN op=(HPEN)SelectObject(dc,hp);
    SelectObject(dc,GetStockObject(NULL_BRUSH)); Rectangle(dc,r.left,r.top,r.right-1,r.bottom-1);
    SelectObject(dc,op); DeleteObject(hp);
    wchar_t txt[64]={}; GetWindowTextW(dis->hwndItem,txt,64);
    HFONT of=(HFONT)SelectObject(dc,st->hBtnFont);
    SetTextColor(dc,kCfgBtnText); SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,txt,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc,of);
}

static LRESULT CALLBACK GameConfigProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* st = (GameCfgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch(msg)
    {
    case WM_CREATE:
    {
        st = (GameCfgState*)((CREATESTRUCTW*)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        st->hTitleFont = CreateFontW(18,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        st->hBtnFont   = CreateFontW(15,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        st->hLblFont   = CreateFontW(14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        st->hBrushBg   = CreateSolidBrush(kCfgBg);
        st->hBrushEdit = CreateSolidBrush(kCfgEditBg);
        st->hBrushGrp  = CreateSolidBrush(kCfgGrpBg);
        HINSTANCE hi = GetModuleHandleW(nullptr);
        constexpr int GX=12, RH=42;
        st->hR[0][0] = CreateWindowExW(0,L"BUTTON",L"2D",   WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, GX+10, 80, 175, RH, hwnd, (HMENU)IDC_CFG_R2D,  hi, nullptr);
        st->hR[0][1] = CreateWindowExW(0,L"BUTTON",L"3D",   WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, GX+200, 80, 175, RH, hwnd, (HMENU)IDC_CFG_R3D,  hi, nullptr);
        st->hR[1][0] = CreateWindowExW(0,L"BUTTON",L"Cua so",        WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, GX+10, 168, 175, RH, hwnd, (HMENU)IDC_CFG_RWIN,  hi, nullptr);
        st->hR[1][1] = CreateWindowExW(0,L"BUTTON",L"Toan man hinh", WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, GX+200, 168, 175, RH, hwnd, (HMENU)IDC_CFG_RFULL, hi, nullptr);
        st->hR[2][0] = CreateWindowExW(0,L"BUTTON",L"800x600",  WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, GX+10, 256, 175, RH, hwnd, (HMENU)IDC_CFG_R800,  hi, nullptr);
        st->hR[2][1] = CreateWindowExW(0,L"BUTTON",L"1024x768", WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, GX+200, 256, 175, RH, hwnd, (HMENU)IDC_CFG_R1024, hi, nullptr);
        st->hScreenEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st->screenshotPath.c_str(),
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL, GX, 352, 280, 26, hwnd, (HMENU)IDC_CFG_SCREDIT, hi, nullptr);
        SendMessageW(st->hScreenEdit, WM_SETFONT, (WPARAM)st->hLblFont, TRUE);
        SetWindowTheme(st->hScreenEdit, L"", L"");
        CreateWindowExW(0,L"BUTTON",L"Thay doi",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, GX+288,352,108,26,hwnd,(HMENU)IDC_CFG_BROWSE,hi,nullptr);
        CreateWindowExW(0,L"BUTTON",L"Luu",   WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,  GX,    398,185,36,hwnd,(HMENU)IDC_CFG_SAVE,  hi,nullptr);
        CreateWindowExW(0,L"BUTTON",L"Huy bo",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,  GX+213,398,185,36,hwnd,(HMENU)IDC_CFG_CANCEL,hi,nullptr);
        return 0;
    }
    case WM_DRAWITEM:
    {
        if(!st) return FALSE;
        auto* dis = (DRAWITEMSTRUCT*)lParam;
        int id = (int)dis->CtlID;
        if(id==IDC_CFG_R2D||id==IDC_CFG_R3D||id==IDC_CFG_RWIN||
           id==IDC_CFG_RFULL||id==IDC_CFG_R800||id==IDC_CFG_R1024)
        { DrawCfgRadio(dis, st); return TRUE; }
        DrawCfgBtn(dis, st); return TRUE;
    }
    case WM_COMMAND:
    {
        if(!st) return 0;
        int id=LOWORD(wParam);
        auto HitRadio=[&](int grp, int opt){
            int&sel=(grp==0)?st->selGraphics:(grp==1)?st->selDisplay:st->selRes;
            sel=opt;
            for(int o=0;o<2;o++) if(st->hR[grp][o]) InvalidateRect(st->hR[grp][o],nullptr,TRUE);
        };
        if(id==IDC_CFG_R2D)  {HitRadio(0,0);return 0;}
        if(id==IDC_CFG_R3D)  {HitRadio(0,1);return 0;}
        if(id==IDC_CFG_RWIN) {HitRadio(1,0);return 0;}
        if(id==IDC_CFG_RFULL){HitRadio(1,1);return 0;}
        if(id==IDC_CFG_R800) {HitRadio(2,0); ApplyResolutionPackage(0); return 0;}
        if(id==IDC_CFG_R1024){HitRadio(2,1); ApplyResolutionPackage(1); return 0;}
        if(id==IDC_CFG_BROWSE){
            BROWSEINFOW bi={};
            bi.hwndOwner=hwnd; bi.lpszTitle=L"Chon thu muc luu screenshot:";
            bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl=SHBrowseForFolderW(&bi);
            if(pidl){wchar_t path[MAX_PATH]={};SHGetPathFromIDListW(pidl,path);SetWindowTextW(st->hScreenEdit,path);CoTaskMemFree(pidl);}
            return 0;
        }
        if(id==IDC_CFG_SAVE){
            st->confirmed=true;
            wchar_t buf[1024]={};GetWindowTextW(st->hScreenEdit,buf,_countof(buf));st->screenshotPath=buf;
            DestroyWindow(hwnd);return 0;
        }
        if(id==IDC_CFG_CANCEL){DestroyWindow(hwnd);return 0;}
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);
        RECT wr;GetClientRect(hwnd,&wr);
        {RECT tr={0,0,wr.right,48};HBRUSH hbr=CreateSolidBrush(kCfgTitleBg);FillRect(dc,&tr,hbr);DeleteObject(hbr);
         HFONT of=(HFONT)SelectObject(dc,st?st->hTitleFont:nullptr);
         SetTextColor(dc,RGB(255,255,255));SetBkMode(dc,TRANSPARENT);
         DrawTextW(dc,L"THIET LAP CAU HINH",-1,&tr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
         if(of)SelectObject(dc,of);}
        HFONT hlbl=st?st->hLblFont:nullptr;
        auto DrawGroup=[&](int y,int h,const wchar_t*label){
            RECT gr={12,y,12+396,y+h};
            HBRUSH hbr=CreateSolidBrush(kCfgGrpBg);FillRect(dc,&gr,hbr);DeleteObject(hbr);
            HPEN hp=CreatePen(PS_SOLID,1,kCfgGrpBrd);HPEN op=(HPEN)SelectObject(dc,hp);
            SelectObject(dc,GetStockObject(NULL_BRUSH));Rectangle(dc,gr.left,gr.top,gr.right,gr.bottom);
            SelectObject(dc,op);DeleteObject(hp);
            RECT lr={gr.left+10,gr.top-9,gr.left+200,gr.top+9};
            HBRUSH lbr=CreateSolidBrush(kCfgBg);FillRect(dc,&lr,lbr);DeleteObject(lbr);
            HFONT of2=(HFONT)SelectObject(dc,hlbl);
            SetTextColor(dc,kCfgText);SetBkMode(dc,TRANSPARENT);
            DrawTextW(dc,label,-1,&lr,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            if(of2)SelectObject(dc,of2);
        };
        DrawGroup(58,  80, L"Do Hoa");
        DrawGroup(146, 80, L"Che do hien thi");
        DrawGroup(234, 80, L"Do phan giai");
        {RECT lr={12,330,410,348};SetTextColor(dc,kCfgText);SetBkMode(dc,TRANSPARENT);
         HFONT of2=(HFONT)SelectObject(dc,hlbl);
         DrawTextW(dc,L"Duong dan luu screenshot:",-1,&lr,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
         if(of2)SelectObject(dc,of2);}
        EndPaint(hwnd,&ps);return 0;
    }
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wParam, kCfgEditBg); SetTextColor((HDC)wParam, RGB(220,200,180));
        return (LRESULT)(st ? st->hBrushEdit : GetStockObject(BLACK_BRUSH));
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wParam, kCfgGrpBg); SetTextColor((HDC)wParam, kCfgText);
        return (LRESULT)(st ? st->hBrushGrp : GetStockObject(BLACK_BRUSH));
    case WM_ERASEBKGND:
    {
        RECT r; GetClientRect(hwnd, &r);
        HBRUSH hbr=CreateSolidBrush(kCfgBg); FillRect((HDC)wParam,&r,hbr); DeleteObject(hbr);
        return 1;
    }
    case WM_DESTROY:
        if(st){
            if(st->hTitleFont)DeleteObject(st->hTitleFont);
            if(st->hBtnFont)  DeleteObject(st->hBtnFont);
            if(st->hLblFont)  DeleteObject(st->hLblFont);
            if(st->hBrushBg)  DeleteObject(st->hBrushBg);
            if(st->hBrushEdit)DeleteObject(st->hBrushEdit);
            if(st->hBrushGrp) DeleteObject(st->hBrushGrp);
            st->done=true;
        }
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wParam,lParam);
}

static void ShowGameConfigDialog(HWND hParent)
{
    static bool reg=false;
    if(!reg){
        WNDCLASSEXW wc={};wc.cbSize=sizeof(wc);wc.lpfnWndProc=GameConfigProc;
        wc.hInstance=GetModuleHandleW(nullptr);wc.hbrBackground=CreateSolidBrush(kCfgBg);
        wc.lpszClassName=L"GameConfigDlg";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        RegisterClassExW(&wc);reg=true;
    }
    GameCfgState st;
    st.selGraphics  = (g_gameConfig.graphics    == L"2D") ? 0 : 1;
    st.selDisplay   = (g_gameConfig.displayMode == L"windowed") ? 0 : 1;
    st.selRes       = (g_gameConfig.resolution  == L"800x600") ? 0 : 1;
    st.screenshotPath = g_gameConfig.screenshotPath;
    constexpr int W=420, H=450;
    RECT pr; GetWindowRect(hParent,&pr);
    HWND hDlg=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_TOPMOST,
        L"GameConfigDlg",L"",WS_POPUP|WS_BORDER,
        pr.left+(pr.right-pr.left-W)/2, pr.top+(pr.bottom-pr.top-H)/2, W,H,
        hParent,nullptr,GetModuleHandleW(nullptr),&st);
    EnableWindow(hParent,FALSE); ShowWindow(hDlg,SW_SHOW);
    MSG msg;
    while(!st.done){
        if(GetMessageW(&msg,nullptr,0,0)==0){PostQuitMessage((int)msg.wParam);break;}
        if(!IsDialogMessageW(hDlg,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    }
    EnableWindow(hParent,TRUE); SetForegroundWindow(hParent);
    if(st.confirmed){
        g_gameConfig.graphics    = st.selGraphics==0 ? L"2D"       : L"3D";
        g_gameConfig.displayMode = st.selDisplay ==0 ? L"windowed"  : L"fullscreen";
        g_gameConfig.resolution  = st.selRes     ==0 ? L"800x600"   : L"1024x768";
        g_gameConfig.screenshotPath = st.screenshotPath;
        SaveGameConfig();
    }
}

// ==========================================================================
//  MAIN WINDOW PROCEDURE
// ==========================================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch(msg){
    case WM_UPDATE_STATUS:{auto*s=(std::wstring*)lParam;SetWindowTextW(g_hwndStatus,s->c_str());delete s;return 0;}
    case WM_UPDATE_PROGRESS:SetBar(g_hwndFileBar,g_hwndFilePct,(int)wParam,1000);SetBar(g_hwndOverallBar,g_hwndOverallPct,(int)lParam,1000);return 0;
    case WM_UPDATE_STARTED:EnableWindow(g_hwndBtnCheck,FALSE);EnableWindow(g_hwndBtnGame,FALSE);return 0;
    case WM_UPDATE_DONE:   EnableWindow(g_hwndBtnCheck,TRUE); EnableWindow(g_hwndBtnGame,TRUE); return 0;
    case WM_COMMAND:{
        int id=LOWORD(wParam),code=HIWORD(wParam);
        if(id>IDC_SERVER_BASE&&id<=IDC_SERVER_BASE+32&&code==BN_CLICKED){
            int sid=id-IDC_SERVER_BASE;bool chk=(SendMessageW((HWND)lParam,BM_GETCHECK,0,0)==BST_CHECKED);
            if(chk){for(auto&s:g_servers)if(s.id!=sid&&s.hCheck)SendMessageW(s.hCheck,BM_SETCHECK,BST_UNCHECKED,0);g_selectedServer=sid;SaveSelectedServer();if(!g_isUpdating)std::thread(RunUpdate).detach();}
            else{g_selectedServer=0;SaveSelectedServer();}
            return 0;
        }
        if(id==IDC_BTN_CHECK){ShowAutoPlayDialog(hwnd);return 0;}
        if(id==IDC_BTN_GAME){
            auto gp=g_exe.dir+L"\\"+g_config.gameExe;
            if(GetFileAttributesW(gp.c_str())==INVALID_FILE_ATTRIBUTES){MessageBoxW(hwnd,(L"Khong tim thay:\n"+gp).c_str(),L"Khong tim thay",MB_ICONWARNING);return 0;}
            ShellExecuteW(nullptr,L"open",gp.c_str(),nullptr,g_exe.dir.c_str(),SW_SHOWNORMAL);return 0;
        }
        if(id==IDC_BTN_CONFIG){ShowGameConfigDialog(hwnd);return 0;}

        // NEW: Mo AutoVLBS
        if(id==IDC_BTN_AUTOVLBS){LaunchExe(g_autoVLBS.exe, hwnd);return 0;}

        // NEW: Mo AutoPK
        if(id==IDC_BTN_AUTOPK){LaunchExe(g_autoPK.exe, hwnd);return 0;}

        return 0;
    }
    case WM_CTLCOLORSTATIC:SetBkColor((HDC)wParam,RGB(28,28,28));SetTextColor((HDC)wParam,RGB(200,200,200));return(LRESULT)g_hBrushBg;
    case WM_CTLCOLORBTN:   SetBkColor((HDC)wParam,RGB(50,50,50));SetTextColor((HDC)wParam,RGB(220,220,220));return(LRESULT)g_hBrushBtn;
    case WM_ERASEBKGND:{RECT r;GetClientRect(hwnd,&r);FillRect((HDC)wParam,&r,g_hBrushBg);return 1;}
    case WM_CLOSE:  DestroyWindow(hwnd);return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wParam,lParam);
}

static HWND MakeBtn(HINSTANCE hi,const wchar_t*t,int id,int x,int y,int w,int h,HWND hp)
{HWND b=CreateWindowExW(0,L"BUTTON",t,WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_FLAT,x,y,w,h,hp,(HMENU)(INT_PTR)id,hi,nullptr);SendMessageW(b,WM_SETFONT,(WPARAM)g_hFont,TRUE);SetWindowTheme(b,L"",L"");return b;}
static HWND MakeCheck(HINSTANCE hi,const wchar_t*t,int id,int x,int y,int w,int h,HWND hp)
{HWND b=CreateWindowExW(0,L"BUTTON",t,WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|BS_FLAT,x,y,w,h,hp,(HMENU)(INT_PTR)id,hi,nullptr);SendMessageW(b,WM_SETFONT,(WPARAM)g_hFontSmall,TRUE);SetWindowTheme(b,L"",L"");return b;}

// ==========================================================================
//  ENTRY POINT
// ==========================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    InitExeInfo();
    DeleteFileW((g_exe.path+L".old").c_str());
    DeleteFileW((g_exe.dir+L"\\"+g_exe.stem+L"_pending.bin").c_str());
    g_configPath=g_exe.dir+L"\\UpdaterConfig.ini";
    LoadConfig();
    g_lay.Compute((int)g_servers.size());

    INITCOMMONCONTROLSEX icc={sizeof(icc),ICC_PROGRESS_CLASS};InitCommonControlsEx(&icc);
    g_hBrushBg  =CreateSolidBrush(RGB(28,28,28));
    g_hBrushBtn =CreateSolidBrush(RGB(50,50,50));
    g_hBrushEdit=CreateSolidBrush(RGB(40,40,40));
    g_hFont     =CreateFontW(15,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
    g_hFontSmall=CreateFontW(13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");

    WNDCLASSEXW wc={};wc.cbSize=sizeof(wc);wc.lpfnWndProc=WndProc;wc.hInstance=hInst;wc.hbrBackground=g_hBrushBg;wc.lpszClassName=L"AutoUpdateWnd";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION);RegisterClassExW(&wc);

    RECT rc={0,0,g_lay.W,g_lay.clientH};AdjustWindowRectEx(&rc,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_APPWINDOW);
    int ww=rc.right-rc.left,wh=rc.bottom-rc.top;
    g_hwnd=CreateWindowExW(WS_EX_APPWINDOW,L"AutoUpdateWnd",L"Game Updater",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,(GetSystemMetrics(SM_CXSCREEN)-ww)/2,(GetSystemMetrics(SM_CYSCREEN)-wh)/2,ww,wh,nullptr,nullptr,hInst,nullptr);

    for(auto&s:g_servers){int cy=10+(s.id-1)*24;std::wstring lbl=std::to_wstring(s.id)+L".  "+s.name+L"  ["+s.url+L"]";s.hCheck=MakeCheck(hInst,lbl.c_str(),IDC_SERVER_BASE+s.id,g_lay.ML,cy,g_lay.W-g_lay.ML-8,20,g_hwnd);if(s.id==g_selectedServer)SendMessageW(s.hCheck,BM_SETCHECK,BST_CHECKED,0);}
    if(g_lay.sepY>=0)g_hwndSep=CreateWindowExW(0,L"STATIC",nullptr,WS_CHILD|WS_VISIBLE|SS_ETCHEDHORZ,g_lay.ML,g_lay.sepY,g_lay.W-g_lay.ML*2,2,g_hwnd,nullptr,hInst,nullptr);

    g_hwndStatus=CreateWindowExW(0,L"STATIC",L"Dang khoi dong...",WS_CHILD|WS_VISIBLE|SS_LEFT,g_lay.ML,g_lay.statusY,g_lay.W-g_lay.MR,g_lay.statusH,g_hwnd,nullptr,hInst,nullptr);SendMessageW(g_hwndStatus,WM_SETFONT,(WPARAM)g_hFont,TRUE);

    g_hwndFileLabel=CreateWindowExW(0,L"STATIC",L"Tep hien tai:",WS_CHILD|WS_VISIBLE|SS_LEFT,g_lay.ML,g_lay.fileLY,120,g_lay.fileLH,g_hwnd,nullptr,hInst,nullptr);SendMessageW(g_hwndFileLabel,WM_SETFONT,(WPARAM)g_hFontSmall,TRUE);
    g_hwndFileBar=CreateWindowExW(0,PROGRESS_CLASSW,nullptr,WS_CHILD|WS_VISIBLE|PBS_SMOOTH,g_lay.ML,g_lay.fileBY,g_lay.barW,g_lay.fileBH,g_hwnd,nullptr,hInst,nullptr);
    SendMessageW(g_hwndFileBar,PBM_SETRANGE32,0,1000);SendMessageW(g_hwndFileBar,PBM_SETBARCOLOR,0,RGB(0,160,230));SendMessageW(g_hwndFileBar,PBM_SETBKCOLOR,0,RGB(55,55,55));
    g_hwndFilePct=CreateWindowExW(0,L"STATIC",L"--",WS_CHILD|WS_VISIBLE|SS_CENTER,g_lay.pctX,g_lay.fileBY,g_lay.pctW,g_lay.fileBH,g_hwnd,nullptr,hInst,nullptr);SendMessageW(g_hwndFilePct,WM_SETFONT,(WPARAM)g_hFontSmall,TRUE);

    g_hwndOverallLabel=CreateWindowExW(0,L"STATIC",L"Tong tien trinh:",WS_CHILD|WS_VISIBLE|SS_LEFT,g_lay.ML,g_lay.ovrLY,140,g_lay.ovrLH,g_hwnd,nullptr,hInst,nullptr);SendMessageW(g_hwndOverallLabel,WM_SETFONT,(WPARAM)g_hFontSmall,TRUE);
    g_hwndOverallBar=CreateWindowExW(0,PROGRESS_CLASSW,nullptr,WS_CHILD|WS_VISIBLE|PBS_SMOOTH,g_lay.ML,g_lay.ovrBY,g_lay.barW,g_lay.ovrBH,g_hwnd,nullptr,hInst,nullptr);
    SendMessageW(g_hwndOverallBar,PBM_SETRANGE32,0,1000);SendMessageW(g_hwndOverallBar,PBM_SETBARCOLOR,0,RGB(0,200,120));SendMessageW(g_hwndOverallBar,PBM_SETBKCOLOR,0,RGB(55,55,55));
    g_hwndOverallPct=CreateWindowExW(0,L"STATIC",L"--",WS_CHILD|WS_VISIBLE|SS_CENTER,g_lay.pctX,g_lay.ovrBY,g_lay.pctW,g_lay.ovrBH,g_hwnd,nullptr,hInst,nullptr);SendMessageW(g_hwndOverallPct,WM_SETFONT,(WPARAM)g_hFontSmall,TRUE);

    // ---- Row 1: Mo Auto | Vao Game | Cau hinh ----------------------------- //
    int B1=g_lay.ML, B2=B1+g_lay.BW+g_lay.BG, B3=B2+g_lay.BW+g_lay.BG;
    g_hwndBtnCheck =MakeBtn(hInst,L"Mo Auto",   IDC_BTN_CHECK, B1,g_lay.btnY,g_lay.BW,g_lay.btnH,g_hwnd);
    g_hwndBtnGame  =MakeBtn(hInst,L"Vao Game",  IDC_BTN_GAME,  B2,g_lay.btnY,g_lay.BW,g_lay.btnH,g_hwnd);
    g_hwndBtnConfig=MakeBtn(hInst,L"Cau hinh",  IDC_BTN_CONFIG,B3,g_lay.btnY,g_lay.BW,g_lay.btnH,g_hwnd);
    EnableWindow(g_hwndBtnGame,FALSE);

    // ---- Row 2: Mo AutoVLBS | Mo AutoPK ----------------------------------- //
    // Use label from INI (g_autoVLBS.label / g_autoPK.label) so server can rename via config
    int R2_B1=g_lay.ML, R2_B2=R2_B1+g_lay.BW2+g_lay.BG;
    g_hwndBtnAutoVLBS=MakeBtn(hInst,g_autoVLBS.label.c_str(),IDC_BTN_AUTOVLBS,R2_B1,g_lay.btn2Y,g_lay.BW2,g_lay.btnH,g_hwnd);
    g_hwndBtnAutoPK  =MakeBtn(hInst,g_autoPK.label.c_str(),  IDC_BTN_AUTOPK,  R2_B2,g_lay.btn2Y,g_lay.BW2,g_lay.btnH,g_hwnd);

    ShowWindow(g_hwnd,SW_SHOW);UpdateWindow(g_hwnd);
    if(g_selectedServer>0&&!GetServerURL().empty())std::thread(RunUpdate).detach();
    else PostStatus(L"Chon server de bat dau cap nhat.");

    MSG msg;while(GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}

    DeleteObject(g_hBrushBg);DeleteObject(g_hBrushBtn);DeleteObject(g_hBrushEdit);
    DeleteObject(g_hFont);DeleteObject(g_hFontSmall);
    if(g_hPopTitle)DeleteObject(g_hPopTitle);if(g_hPopBtn)DeleteObject(g_hPopBtn);
    return 0;
}
