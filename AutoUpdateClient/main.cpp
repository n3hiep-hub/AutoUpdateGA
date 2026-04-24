// AutoUpdateClient — Game launcher với asset-driven skin system
// Tích hợp SkinEngine.h: background PNG, button PNG 3-state, logo, progress bar
// Mọi ảnh + layout đọc từ UpdaterConfig.ini → không cần recompile khi đổi skin

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
#include "SkinEngine.h"        // <-- skin system mới

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "ole32.lib")

static const wchar_t* kDefaultBaseURL = L"http://localhost/Update";
static const wchar_t* kDefaultGameExe = L"Game.exe";

// ---- Control IDs ----------------------------------------------------------
#define IDC_BTN_CHECK       101
#define IDC_BTN_GAME        102
#define IDC_BTN_CONFIG      103
#define IDC_BTN_AUTOVLBS    104
#define IDC_BTN_AUTOPK      105
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

// ---- Window messages -------------------------------------------------------
#define WM_UPDATE_STATUS    (WM_USER + 1)
#define WM_UPDATE_PROGRESS  (WM_USER + 2)
#define WM_UPDATE_DONE      (WM_USER + 3)
#define WM_UPDATE_STARTED   (WM_USER + 4)

// ---- Data structs ----------------------------------------------------------
struct ServerEntry    { int id; std::wstring name, url; HWND hCheck = nullptr; };
struct AutoPlayEntry  { int id; std::wstring text, exe; };
struct QuickLaunch    { std::wstring exe; std::wstring label; };
struct GameConfig
{
    std::wstring graphics, displayMode, resolution, screenshotPath;
    std::wstring packageDir, packageDestFile;
};
struct AppConfig { std::wstring gameExe; };

static std::vector<ServerEntry>   g_servers;
static int                        g_selectedServer = 0;
static std::vector<AutoPlayEntry> g_autoPlay;
static std::wstring               g_fontDir, g_fontText;
static QuickLaunch                g_autoVLBS, g_autoPK;
static GameConfig                 g_gameConfig;
static AppConfig                  g_config;
static std::wstring               g_configPath;
static std::atomic<bool>          g_isUpdating{false};

// ---- Layout (tính từ INI + server count) -----------------------------------
struct LayoutData
{
    // Chiều rộng lấy từ g_skin.winW, chiều cao từ g_skin.winH
    // Các vị trí button, progress bar tính theo tỉ lệ hoặc đọc từ INI
    int W = 520, H = 460;
    int ML = 12, MR = 40;

    // Button row 1: 3 nút
    int btn1Y = 0, btnH = 42, btnW1 = 0;
    // Button row 2: 2 nút
    int btn2Y = 0, btnW2 = 0;
    int BG = 8;

    // Server list area
    int serverStartY = 8;
    int serverRowH   = 22;
    int sepY         = -1;

    // Status + progress
    int statusY = 0, statusH = 18;
    int fileBarLY = 0, fileBarY = 0, fileBarH = 10;
    int ovrBarLY  = 0, ovrBarY  = 0, ovrBarH  = 10;
    int pctX      = 0, pctW     = 32;

    void Compute(int nServers)
    {
        W = g_skin.winW;
        H = g_skin.winH;

        int y = serverStartY;
        if (nServers > 0) {
            y += nServers * serverRowH;
            sepY = y + 4; y = sepY + 2 + 8;
        }
        statusY = y; y += statusH + 6;

        // progress bars — lấy từ skin hoặc tính
        fileBarLY = y; y += 14;
        fileBarY  = y; fileBarH = g_skin.barH > 0 ? g_skin.barH : 10;
        y += fileBarH + 6;

        ovrBarLY = y; y += 14;
        ovrBarY  = y; ovrBarH  = fileBarH;
        y += ovrBarH + 10;

        // Cập nhật barY vào skin để DrawProgressBar dùng
        g_skin.barX = ML;
        g_skin.barW = W - ML - MR;
        g_skin.barY  = fileBarY;
        g_skin.bar2Y = ovrBarY;

        pctX = W - MR + 4; pctW = 32;

        // Button rows
        btn1Y = y; y += btnH + BG;
        btn2Y = y;

        // Button widths chia đều
        int available = W - ML * 2;
        btnW1 = (available - 2 * BG) / 3;   // 3 nút row 1
        btnW2 = (available - BG)     / 2;   // 2 nút row 2

        H = y + btnH + 14;   // tự điều chỉnh chiều cao nếu INI không set
        if (g_skin.winH > 100) H = g_skin.winH; // INI override
    }
};
static LayoutData g_lay;

// ---- UI handles ------------------------------------------------------------
static HWND g_hwnd=nullptr, g_hwndSep=nullptr, g_hwndStatus=nullptr;
static HWND g_hwndFileLabel=nullptr, g_hwndFilePct=nullptr;
static HWND g_hwndOvrLabel=nullptr,  g_hwndOvrPct=nullptr;
static HWND g_hwndBtnCheck=nullptr,  g_hwndBtnGame=nullptr, g_hwndBtnConfig=nullptr;
static HWND g_hwndBtnAutoVLBS=nullptr, g_hwndBtnAutoPK=nullptr;

// Progress values (vẽ thủ công, không dùng PROGRESS_CLASS)
static int g_fileVal=0, g_ovrVal=0;

// ---- Exe info --------------------------------------------------------------
struct ExeInfo { std::wstring path, dir, filename, stem; };
static ExeInfo g_exe;

static void InitExeInfo()
{
    wchar_t buf[MAX_PATH]={};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    g_exe.path = buf;
    auto sl = g_exe.path.rfind(L'\\');
    if (sl != std::wstring::npos) {
        g_exe.dir      = g_exe.path.substr(0, sl);
        g_exe.filename = g_exe.path.substr(sl + 1);
    } else { g_exe.dir = L"."; g_exe.filename = g_exe.path; }
    auto dt = g_exe.filename.rfind(L'.');
    g_exe.stem = (dt != std::wstring::npos) ? g_exe.filename.substr(0, dt) : g_exe.filename;
}

// ---- Config ----------------------------------------------------------------
static void LoadConfig()
{
    wchar_t buf[1024]={};
    auto ri = [&](const wchar_t* s, const wchar_t* k, int d) {
        GetPrivateProfileStringW(s,k,std::to_wstring(d).c_str(),buf,_countof(buf),g_configPath.c_str());
        return _wtoi(buf);
    };
    auto rs = [&](const wchar_t* s, const wchar_t* k, const wchar_t* d) -> std::wstring {
        GetPrivateProfileStringW(s,k,d,buf,_countof(buf),g_configPath.c_str());
        return buf;
    };

    // [ServerList]
    g_servers.clear();
    for (int i=1; i<=32; ++i) {
        auto name = rs(L"ServerList",(std::to_wstring(i)+L"_Server").c_str(),L"");
        if (name.empty()) break;
        ServerEntry se; se.id=i; se.name=name;
        se.url = rs(L"ServerList",(std::to_wstring(i)+L"_host").c_str(),kDefaultBaseURL);
        g_servers.push_back(se);
    }
    g_selectedServer = ri(L"LastSel",L"PicSel",0);

    // [AutoPlay]
    g_autoPlay.clear();
    for (int i=1; i<=32; ++i) {
        auto txt = rs(L"AutoPlay",(std::to_wstring(i)+L"_Text").c_str(),L"");
        if (txt.empty()) break;
        AutoPlayEntry e; e.id=i; e.text=txt;
        e.exe = rs(L"AutoPlay",(std::to_wstring(i)+L"_Exe").c_str(),L"");
        g_autoPlay.push_back(e);
    }
    g_fontDir  = rs(L"FontEdit",L"FontDir",L"");
    g_fontText = rs(L"FontEdit",L"FontText",L"Sua loi Font chu");

    // [GameConfig]
    g_gameConfig.graphics        = rs(L"GameConfig",L"Graphics",L"3D");
    g_gameConfig.displayMode     = rs(L"GameConfig",L"DisplayMode",L"fullscreen");
    g_gameConfig.resolution      = rs(L"GameConfig",L"Resolution",L"1024x768");
    g_gameConfig.screenshotPath  = rs(L"GameConfig",L"ScreenshotPath",L"");
    g_gameConfig.packageDir      = rs(L"GameConfig",L"PackageDir",L"Package");
    g_gameConfig.packageDestFile = rs(L"GameConfig",L"PackageDestFile",L"package.ini");

    // [Updater]
    g_config.gameExe = rs(L"Updater",L"GameExe",kDefaultGameExe);

    // [AutoVLBS] / [AutoPK]
    g_autoVLBS.exe   = rs(L"AutoVLBS",L"Exe",L"");
    g_autoVLBS.label = rs(L"AutoVLBS",L"Label",L"Mo AutoVLBS");
    g_autoPK.exe     = rs(L"AutoPK",  L"Exe",L"");
    g_autoPK.label   = rs(L"AutoPK",  L"Label",L"Mo AutoPK");
}

static void SaveSelectedServer()
{
    WritePrivateProfileStringW(L"LastSel",L"PicSel",
        std::to_wstring(g_selectedServer).c_str(), g_configPath.c_str());
}
static void SaveGameConfig()
{
    WritePrivateProfileStringW(L"GameConfig",L"Graphics",   g_gameConfig.graphics.c_str(),    g_configPath.c_str());
    WritePrivateProfileStringW(L"GameConfig",L"DisplayMode",g_gameConfig.displayMode.c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"GameConfig",L"Resolution", g_gameConfig.resolution.c_str(),  g_configPath.c_str());
    WritePrivateProfileStringW(L"GameConfig",L"ScreenshotPath",g_gameConfig.screenshotPath.c_str(),g_configPath.c_str());
}

static std::wstring GetServerURL()
{
    for (const auto& s : g_servers) if (s.id == g_selectedServer) return s.url;
    if (g_servers.empty()) return kDefaultBaseURL;
    return {};
}

static void LaunchExe(const std::wstring& exeName, HWND hParent)
{
    if (exeName.empty()) {
        MessageBoxW(hParent,L"Chua cau hinh exe trong UpdaterConfig.ini.",L"Thong bao",MB_ICONINFORMATION);
        return;
    }
    std::wstring ep = g_exe.dir + L"\\" + exeName;
    for (auto& c : ep) if (c==L'/') c=L'\\';
    if (GetFileAttributesW(ep.c_str())==INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hParent,(L"Khong tim thay:\n"+ep).c_str(),L"Khong tim thay",MB_ICONWARNING);
        return;
    }
    ShellExecuteW(nullptr,L"open",ep.c_str(),nullptr,g_exe.dir.c_str(),SW_SHOWNORMAL);
}

// ---- INI parser ------------------------------------------------------------
struct FileEntry { std::string relativePath, expectedMD5; };
static std::vector<FileEntry> ParseIni(const std::string& text, std::wstring& outBase, const std::wstring& fallback)
{
    outBase = fallback;
    std::vector<FileEntry> entries;
    std::istringstream ss(text); std::string line; bool inF=false;
    while (std::getline(ss,line)) {
        if (!line.empty()&&line.back()=='\r') line.pop_back();
        if (line.empty()||line[0]==';') continue;
        if (line[0]=='[') { inF=(line.substr(1,line.find(']')-1)=="Files"); continue; }
        auto eq=line.find('='); if (eq==std::string::npos) continue;
        std::string k=line.substr(0,eq), v=line.substr(eq+1);
        if (!inF) { if(k=="BaseURL"&&!v.empty()) outBase={v.begin(),v.end()}; continue; }
        FileEntry fe; fe.relativePath=k; fe.expectedMD5=v;
        for (auto& c:fe.expectedMD5) c=(char)tolower((unsigned char)c);
        entries.push_back(std::move(fe));
    }
    return entries;
}
static std::wstring ToLocal(const std::string& r){std::wstring w(r.begin(),r.end());for(auto&c:w)if(c==L'/')c=L'\\';return w;}
static std::wstring ToURL(const std::string& r)  {std::wstring w(r.begin(),r.end());for(auto&c:w)if(c==L'\\')c=L'/';return w;}
static std::wstring MakeURL(const std::wstring& base,const std::string& rel)
{
    std::wstring u=base;
    if(!u.empty()&&u.back()==L'/') u.pop_back();
    u+=L'/'; u+=ToURL(rel); return u;
}

// ---- UI helpers ------------------------------------------------------------
static void PostStatus(const std::wstring& msg)
{
    PostMessageW(g_hwnd,WM_UPDATE_STATUS,0,(LPARAM)(new std::wstring(msg)));
}
static void PostProgress(int f, int o)
{
    PostMessageW(g_hwnd,WM_UPDATE_PROGRESS,(WPARAM)f,(LPARAM)o);
}

// ---- Self-update -----------------------------------------------------------
static bool ApplySelfUpdate(const std::wstring& pending)
{
    auto bak=g_exe.path+L".old"; DeleteFileW(bak.c_str());
    if(!MoveFileW(g_exe.path.c_str(),bak.c_str())) return false;
    if(!MoveFileW(pending.c_str(),g_exe.path.c_str())) {
        MoveFileW(bak.c_str(),g_exe.path.c_str());
        DeleteFileW(pending.c_str()); return false;
    }
    return true;
}

// ==========================================================================
//  UPDATE WORKER
// ==========================================================================
static void RunUpdate()
{
    g_isUpdating=true; PostMessageW(g_hwnd,WM_UPDATE_STARTED,0,0);
    auto srv=GetServerURL();
    if(srv.empty()){PostStatus(L"Vui long chon server truoc.");g_isUpdating=false;PostMessageW(g_hwnd,WM_UPDATE_DONE,1,0);return;}
    PostStatus(L"Dang tai danh sach cap nhat..."); PostProgress(0,0);
    auto dl=AutoUpdate::DownloadToMemory(MakeURL(srv,"AutoUpdate.ini"));
    if(!dl.success||dl.content.empty()){
        MessageBoxW(nullptr,(L"Khong tai duoc AutoUpdate.ini tu:\n"+srv).c_str(),L"Loi",MB_ICONERROR);
        g_isUpdating=false; PostMessageW(g_hwnd,WM_UPDATE_DONE,0,0); return;
    }
    std::wstring base; auto entries=ParseIni(dl.content,base,srv);
    PostStatus(L"Dang kiem tra file...");
    std::vector<const FileEntry*> toU; const FileEntry* selfE=nullptr;
    for(const auto&e:entries){
        auto rel=ToLocal(e.relativePath);
        if(_wcsicmp(rel.c_str(),g_exe.filename.c_str())==0){
            if(_stricmp(AutoUpdate::ComputeFileMD5(g_exe.path).c_str(),e.expectedMD5.c_str())!=0) selfE=&e;
            continue;
        }
        auto lp=g_exe.dir+L"\\"+rel;
        bool need=(GetFileAttributesW(lp.c_str())==INVALID_FILE_ATTRIBUTES);
        if(!need) need=(_stricmp(AutoUpdate::ComputeFileMD5(lp).c_str(),e.expectedMD5.c_str())!=0);
        if(need) toU.push_back(&e);
    }
    int total=(int)toU.size()+(selfE?1:0);
    if(total==0){PostStatus(L"Game da o phien ban moi nhat!");PostProgress(1000,1000);g_isUpdating=false;PostMessageW(g_hwnd,WM_UPDATE_DONE,1,0);return;}
    DWORD lastTick=0;
    for(int i=0;i<(int)toU.size();++i){
        const FileEntry*e=toU[i]; std::wstring rw(e->relativePath.begin(),e->relativePath.end());
        PostStatus(L"Dang tai ("+std::to_wstring(i+1)+L"/"+std::to_wstring(total)+L")  "+rw);
        auto dest=g_exe.dir+L"\\"+ToLocal(e->relativePath);
        auto cb=[&](DWORD64 r,DWORD64 t){
            if(GetTickCount()-lastTick<50)return;lastTick=GetTickCount();
            int fp=t?(int)(r*1000ull/t):500,op=(int)(((DWORD64)i*1000ull+fp)/(DWORD64)total);
            PostProgress(fp,op);
        };
        bool ok=AutoUpdate::DownloadToFile(MakeURL(base,e->relativePath),dest,cb);
        if(!ok){Sleep(1500);ok=AutoUpdate::DownloadToFile(MakeURL(base,e->relativePath),dest,nullptr);}
        if(!ok){MessageBoxW(nullptr,(L"Khong tai duoc:\n"+rw).c_str(),L"Loi",MB_ICONERROR);g_isUpdating=false;PostMessageW(g_hwnd,WM_UPDATE_DONE,0,0);return;}
        PostProgress(1000,(int)((DWORD64)(i+1)*1000ull/(DWORD64)toU.size()));
    }
    if(selfE){
        PostStatus(L"Dang tai ban moi AutoUpdateClient...");
        auto tmp=g_exe.dir+L"\\"+g_exe.stem+L"_pending.bin"; DeleteFileW(tmp.c_str());
        bool ok=AutoUpdate::DownloadToFile(MakeURL(base,selfE->relativePath),tmp,nullptr);
        if(!ok){MessageBoxW(nullptr,L"Khong tai duoc ban moi AutoUpdateClient.",L"Loi",MB_ICONWARNING);g_isUpdating=false;PostMessageW(g_hwnd,WM_UPDATE_DONE,1,0);return;}
        PostProgress(1000,1000);
        if(ApplySelfUpdate(tmp)){ShellExecuteW(nullptr,L"open",g_exe.path.c_str(),nullptr,g_exe.dir.c_str(),SW_SHOWNORMAL);PostMessageW(g_hwnd,WM_CLOSE,0,0);}
        else{DeleteFileW(tmp.c_str());MessageBoxW(nullptr,L"Khong the thay the AutoUpdateClient.exe.",L"Loi",MB_ICONWARNING);g_isUpdating=false;PostMessageW(g_hwnd,WM_UPDATE_DONE,1,0);}
        return;
    }
    PostStatus(L"Cap nhat hoan tat!"); PostProgress(1000,1000);
    g_isUpdating=false; PostMessageW(g_hwnd,WM_UPDATE_DONE,1,0);
}

// ==========================================================================
//  AUTOPLAY POPUP  (giữ nguyên từ bản cũ)
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
        if(id==IDC_FONT_BTN&&!g_fontDir.empty()){std::wstring p=g_exe.dir+L"\\"+g_fontDir;ShellExecuteW(nullptr,L"open",p.c_str(),nullptr,g_exe.dir.c_str(),SW_SHOWNORMAL);return 0;}
        if(id>IDC_AUTO_BASE&&id<=IDC_AUTO_BASE+32){int idx=id-IDC_AUTO_BASE;for(const auto&e:g_autoPlay){if(e.id==idx){if(!e.exe.empty()){std::wstring ep=g_exe.dir+L"\\"+e.exe;ShellExecuteW(nullptr,L"open",ep.c_str(),nullptr,g_exe.dir.c_str(),SW_SHOWNORMAL);}break;}}return 0;}
        return 0;
    }
    case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);RECT wr;GetClientRect(hwnd,&wr);RECT tr={0,0,wr.right,55};HBRUSH hbr=CreateSolidBrush(kTitleBg);FillRect(dc,&tr,hbr);DeleteObject(hbr);HFONT of=(HFONT)SelectObject(dc,g_hPopTitle);SetTextColor(dc,RGB(255,220,170));SetBkMode(dc,TRANSPARENT);DrawTextW(dc,L"LUA CHON AUTO",-1,&tr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,of);EndPaint(hwnd,&ps);return 0;}
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
    HWND hDlg=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_TOPMOST,L"AutoPlayDlg",L"",WS_POPUP|WS_BORDER,pr.left+(pr.right-pr.left-W)/2,pr.top+(pr.bottom-pr.top-H)/2,W,H,hParent,nullptr,GetModuleHandleW(nullptr),&done);
    EnableWindow(hParent,FALSE);ShowWindow(hDlg,SW_SHOW);
    MSG msg;while(!done){if(GetMessageW(&msg,nullptr,0,0)==0){PostQuitMessage((int)msg.wParam);break;}if(!IsDialogMessageW(hDlg,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    EnableWindow(hParent,TRUE);SetForegroundWindow(hParent);
}

// ==========================================================================
//  MAIN WINDOW PROCEDURE
// ==========================================================================

// Font cho status text + server list
static HFONT g_hFont=nullptr, g_hFontSmall=nullptr;

// Map HWND button → IDC để track hover/press
static std::unordered_map<int,HWND> g_btnMap; // IDC → HWND

static void InvalidateBtn(HWND h) { if(h) InvalidateRect(h,nullptr,FALSE); }

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch(msg)
    {
    // --- Update messages ---
    case WM_UPDATE_STATUS:
    {
        auto*s=(std::wstring*)lParam;
        SetWindowTextW(g_hwndStatus,s->c_str());
        delete s; return 0;
    }
    case WM_UPDATE_PROGRESS:
    {
        g_fileVal=(int)wParam; g_ovrVal=(int)lParam;
        // Vẽ lại vùng progress bar
        RECT r={g_lay.ML, g_lay.fileBarY-2,
                g_lay.ML+g_skin.barW+g_lay.MR, g_lay.ovrBarY+g_lay.ovrBarH+2};
        InvalidateRect(hwnd,&r,FALSE);

        // Update % text
        auto pct=[](int v)->std::wstring{wchar_t b[16];swprintf_s(b,L"%d%%",(int)(100LL*v/1000));return b;};
        SetWindowTextW(g_hwndFilePct,pct(g_fileVal).c_str());
        SetWindowTextW(g_hwndOvrPct, pct(g_ovrVal).c_str());
        return 0;
    }
    case WM_UPDATE_STARTED:
        EnableWindow(g_hwndBtnCheck,FALSE);
        EnableWindow(g_hwndBtnGame,FALSE);
        return 0;
    case WM_UPDATE_DONE:
        EnableWindow(g_hwndBtnCheck,TRUE);
        EnableWindow(g_hwndBtnGame,TRUE);
        return 0;

    // --- Button commands ---
    case WM_COMMAND:
    {
        int id=LOWORD(wParam),code=HIWORD(wParam);
        // Server checkboxes
        if(id>IDC_SERVER_BASE&&id<=IDC_SERVER_BASE+32&&code==BN_CLICKED){
            int sid=id-IDC_SERVER_BASE;
            bool chk=(SendMessageW((HWND)lParam,BM_GETCHECK,0,0)==BST_CHECKED);
            if(chk){
                for(auto&s:g_servers)if(s.id!=sid&&s.hCheck)SendMessageW(s.hCheck,BM_SETCHECK,BST_UNCHECKED,0);
                g_selectedServer=sid;SaveSelectedServer();
                if(!g_isUpdating)std::thread(RunUpdate).detach();
            }else{g_selectedServer=0;SaveSelectedServer();}
            return 0;
        }
        if(id==IDC_BTN_CHECK)   {ShowAutoPlayDialog(hwnd);return 0;}
        if(id==IDC_BTN_GAME)    {
            auto gp=g_exe.dir+L"\\"+g_config.gameExe;
            if(GetFileAttributesW(gp.c_str())==INVALID_FILE_ATTRIBUTES){
                MessageBoxW(hwnd,(L"Khong tim thay:\n"+gp).c_str(),L"Khong tim thay",MB_ICONWARNING);
                return 0;
            }
            ShellExecuteW(nullptr,L"open",gp.c_str(),nullptr,g_exe.dir.c_str(),SW_SHOWNORMAL);
            return 0;
        }
        if(id==IDC_BTN_AUTOVLBS){LaunchExe(g_autoVLBS.exe,hwnd);return 0;}
        if(id==IDC_BTN_AUTOPK)  {LaunchExe(g_autoPK.exe,  hwnd);return 0;}
        return 0;
    }

    // --- Owner-draw buttons: dùng SkinEngine ---
    case WM_DRAWITEM:
    {
        auto*dis=(DRAWITEMSTRUCT*)lParam;
        int id=(int)dis->CtlID;
        // Chỉ xử lý 5 button chính, bỏ qua server checkboxes
        if(id==IDC_BTN_CHECK||id==IDC_BTN_GAME||id==IDC_BTN_CONFIG||
           id==IDC_BTN_AUTOVLBS||id==IDC_BTN_AUTOPK)
        {
            int state = g_skin.GetButtonState(dis->hwndItem);
            g_skin.DrawButton(dis, state);
            return TRUE;
        }
        return FALSE;
    }

    // --- Hover tracking cho button ---
    case WM_MOUSEMOVE:
    {
        POINT pt={(short)LOWORD(lParam),(short)HIWORD(lParam)};
        HWND hHit=ChildWindowFromPoint(hwnd,pt);
        if(hHit!=g_skin.hHovered){
            HWND old=g_skin.hHovered;
            g_skin.hHovered=hHit;
            InvalidateBtn(old); InvalidateBtn(hHit);
            // Request MOUSELEAVE để reset hover khi chuột ra ngoài
            TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,hwnd,0};
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        HWND old=g_skin.hHovered;
        g_skin.hHovered=nullptr;
        InvalidateBtn(old);
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        POINT pt={(short)LOWORD(lParam),(short)HIWORD(lParam)};
        g_skin.hPressed=ChildWindowFromPoint(hwnd,pt);
        InvalidateBtn(g_skin.hPressed);
        SetCapture(hwnd); return 0;
    }
    case WM_LBUTTONUP:
    {
        HWND old=g_skin.hPressed;
        g_skin.hPressed=nullptr;
        InvalidateBtn(old);
        ReleaseCapture(); return 0;
    }

    // --- Background: vẽ PNG ---
    case WM_ERASEBKGND:
    {
        RECT r; GetClientRect(hwnd,&r);
        g_skin.DrawBackground((HDC)wParam, r.right, r.bottom);
        return 1;
    }

    // --- Paint: logo + progress bar (các control tự vẽ qua WM_DRAWITEM) ---
    case WM_PAINT:
    {
        PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps);

        // Logo
        g_skin.DrawLogo(dc);

        // Progress bar 1 (file)
        g_skin.DrawProgressBar(dc,
            g_lay.ML, g_lay.fileBarY,
            g_skin.barW, g_lay.fileBarH,
            g_fileVal, 1000);

        // Progress bar 2 (overall)
        g_skin.DrawProgressBar(dc,
            g_lay.ML, g_lay.ovrBarY,
            g_skin.barW, g_lay.ovrBarH,
            g_ovrVal, 1000);

        EndPaint(hwnd,&ps);
        return 0;
    }

    // --- Color cho static text (status, labels) ---
    case WM_CTLCOLORSTATIC:
    {
        SetBkMode((HDC)wParam, TRANSPARENT);
        SetTextColor((HDC)wParam, RGB(210,190,255));
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }

    case WM_CLOSE:  DestroyWindow(hwnd); return 0;
    case WM_DESTROY:PostQuitMessage(0);  return 0;
    }
    return DefWindowProcW(hwnd,msg,wParam,lParam);
}

// Helper tạo button BS_OWNERDRAW (để SkinEngine vẽ)
static HWND MakeSkinBtn(HINSTANCE hi, const wchar_t* t, int id,
                         int x, int y, int w, int h, HWND hp)
{
    HWND b = CreateWindowExW(0, L"BUTTON", t,
        WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
        x, y, w, h, hp, (HMENU)(INT_PTR)id, hi, nullptr);
    // Font sẽ do SkinEngine vẽ trong DrawButton
    return b;
}

static HWND MakeCheck(HINSTANCE hi, const wchar_t* t, int id,
                       int x, int y, int w, int h, HWND hp)
{
    HWND b = CreateWindowExW(0, L"BUTTON", t,
        WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|BS_FLAT,
        x, y, w, h, hp, (HMENU)(INT_PTR)id, hi, nullptr);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
    SetWindowTheme(b, L"", L"");
    return b;
}

// ==========================================================================
//  ENTRY POINT
// ==========================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    InitExeInfo();
    DeleteFileW((g_exe.path+L".old").c_str());
    DeleteFileW((g_exe.dir+L"\\"+g_exe.stem+L"_pending.bin").c_str());
    g_configPath = g_exe.dir + L"\\UpdaterConfig.ini";

    // 1. GDI+ khởi động TRƯỚC khi LoadConfig (vì skin load PNG)
    GdiplusInit();

    // 2. Load config + skin
    LoadConfig();
    g_skin.Load(g_configPath, g_exe.dir);

    // 3. Tính layout
    g_lay.Compute((int)g_servers.size());

    // 4. Font cho static text
    INITCOMMONCONTROLSEX icc={sizeof(icc),ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    g_hFont      = CreateFontW(14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
    g_hFontSmall = CreateFontW(12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");

    // 5. Đăng ký window class
    WNDCLASSEXW wc={};
    wc.cbSize=sizeof(wc); wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName=L"GameLauncherWnd";
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION);
    RegisterClassExW(&wc);

    // 6. Tạo cửa sổ với kích thước từ INI/skin
    int wW=g_lay.W, wH=g_lay.H;
    RECT rc={0,0,wW,wH};
    AdjustWindowRectEx(&rc,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_APPWINDOW);
    int fw=rc.right-rc.left, fh=rc.bottom-rc.top;
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW,
        L"GameLauncherWnd", L"Game Launcher",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        (GetSystemMetrics(SM_CXSCREEN)-fw)/2,
        (GetSystemMetrics(SM_CYSCREEN)-fh)/2,
        fw, fh, nullptr, nullptr, hInst, nullptr);

    // 7. Tạo server checkboxes
    for (auto& s : g_servers) {
        int cy = g_lay.serverStartY + (s.id-1)*g_lay.serverRowH;
        std::wstring lbl = std::to_wstring(s.id)+L".  "+s.name+L"  ["+s.url+L"]";
        s.hCheck = MakeCheck(hInst, lbl.c_str(), IDC_SERVER_BASE+s.id,
                             g_lay.ML, cy, g_lay.W-g_lay.ML-8, 18, g_hwnd);
        if (s.id==g_selectedServer) SendMessageW(s.hCheck,BM_SETCHECK,BST_CHECKED,0);
    }

    // Separator (vẽ thủ công qua WM_PAINT hoặc dùng static)
    if (g_lay.sepY >= 0)
        CreateWindowExW(0,L"STATIC",nullptr,WS_CHILD|WS_VISIBLE|SS_ETCHEDHORZ,
            g_lay.ML, g_lay.sepY, g_lay.W-g_lay.ML*2, 2, g_hwnd, nullptr, hInst, nullptr);

    // 8. Status text
    g_hwndStatus = CreateWindowExW(0,L"STATIC",L"Dang khoi dong...",
        WS_CHILD|WS_VISIBLE|SS_LEFT,
        g_lay.ML, g_lay.statusY, g_lay.W-g_lay.MR, g_lay.statusH,
        g_hwnd, nullptr, hInst, nullptr);
    SendMessageW(g_hwndStatus,WM_SETFONT,(WPARAM)g_hFont,TRUE);

    // Labels progress (vẫn dùng static text)
    g_hwndFileLabel = CreateWindowExW(0,L"STATIC",L"Tep hien tai:",
        WS_CHILD|WS_VISIBLE|SS_LEFT,
        g_lay.ML, g_lay.fileBarLY, 120, 14, g_hwnd, nullptr, hInst, nullptr);
    SendMessageW(g_hwndFileLabel,WM_SETFONT,(WPARAM)g_hFontSmall,TRUE);

    g_hwndFilePct = CreateWindowExW(0,L"STATIC",L"--",
        WS_CHILD|WS_VISIBLE|SS_CENTER,
        g_lay.pctX, g_lay.fileBarY, g_lay.pctW, g_lay.fileBarH,
        g_hwnd, nullptr, hInst, nullptr);
    SendMessageW(g_hwndFilePct,WM_SETFONT,(WPARAM)g_hFontSmall,TRUE);

    g_hwndOvrLabel = CreateWindowExW(0,L"STATIC",L"Tong tien trinh:",
        WS_CHILD|WS_VISIBLE|SS_LEFT,
        g_lay.ML, g_lay.ovrBarLY, 140, 14, g_hwnd, nullptr, hInst, nullptr);
    SendMessageW(g_hwndOvrLabel,WM_SETFONT,(WPARAM)g_hFontSmall,TRUE);

    g_hwndOvrPct = CreateWindowExW(0,L"STATIC",L"--",
        WS_CHILD|WS_VISIBLE|SS_CENTER,
        g_lay.pctX, g_lay.ovrBarY, g_lay.pctW, g_lay.ovrBarH,
        g_hwnd, nullptr, hInst, nullptr);
    SendMessageW(g_hwndOvrPct,WM_SETFONT,(WPARAM)g_hFontSmall,TRUE);

    // 9. Button row 1 (BS_OWNERDRAW — SkinEngine vẽ)
    int B1=g_lay.ML, B2=B1+g_lay.btnW1+g_lay.BG, B3=B2+g_lay.btnW1+g_lay.BG;
    g_hwndBtnCheck  = MakeSkinBtn(hInst,g_autoPlay.empty()?L"Mo Auto":L"Mo Auto",
                                   IDC_BTN_CHECK,  B1,g_lay.btn1Y,g_lay.btnW1,g_lay.btnH,g_hwnd);
    g_hwndBtnGame   = MakeSkinBtn(hInst,L"Vao Game",
                                   IDC_BTN_GAME,   B2,g_lay.btn1Y,g_lay.btnW1,g_lay.btnH,g_hwnd);
    g_hwndBtnConfig = MakeSkinBtn(hInst,L"Cau hinh",
                                   IDC_BTN_CONFIG, B3,g_lay.btn1Y,g_lay.btnW1,g_lay.btnH,g_hwnd);
    EnableWindow(g_hwndBtnGame,FALSE);

    // 10. Button row 2
    int R2B1=g_lay.ML, R2B2=R2B1+g_lay.btnW2+g_lay.BG;
    g_hwndBtnAutoVLBS = MakeSkinBtn(hInst,g_autoVLBS.label.c_str(),
                                     IDC_BTN_AUTOVLBS,R2B1,g_lay.btn2Y,g_lay.btnW2,g_lay.btnH,g_hwnd);
    g_hwndBtnAutoPK   = MakeSkinBtn(hInst,g_autoPK.label.c_str(),
                                     IDC_BTN_AUTOPK,  R2B2,g_lay.btn2Y,g_lay.btnW2,g_lay.btnH,g_hwnd);

    // 11. Show + start update
    ShowWindow(g_hwnd,SW_SHOW); UpdateWindow(g_hwnd);
    if (g_selectedServer>0 && !GetServerURL().empty())
        std::thread(RunUpdate).detach();
    else
        PostStatus(L"Chon server de bat dau cap nhat.");

    // 12. Message loop
    MSG msg;
    while (GetMessageW(&msg,nullptr,0,0)) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }

    // 13. Cleanup
    GdiplusShutdown();
    DeleteObject(g_hFont); DeleteObject(g_hFontSmall);
    if(g_hPopTitle)DeleteObject(g_hPopTitle);
    if(g_hPopBtn)  DeleteObject(g_hPopBtn);
    return 0;
}
