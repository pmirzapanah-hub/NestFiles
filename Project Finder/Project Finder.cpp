// OpenFolderTest.cpp
// Win32 GUI - borderless compact toolbar with fancy icons, auto-width,
// themed Settings window (including buttons) and themed popup menus.
//
// Current features included:
// - Main bar: Project input supports 4 digits or full B00#### (enter opens folder)
// - Folder dropdown: lists subfolders in resolved project folder (or Base Path when Search-anywhere is ON)
// - Recent dropdown: last 10 opened folders/subfolders
// - Search rule: project folder match uses ONLY first 7 chars (B00####) within range folder (when Search-anywhere is OFF)
// - Settings: checkbox "Search anywhere (folder name)"
//   - OFF: original job-number logic (B00#### within range folder)
//   - ON : ignore job-number logic; recursively search Base Path (all subfolders) by folder name contains typed text
//          If multiple matches -> show option menu.
// - NEW changes requested:
//   1) Remove default path (no built-in default; user sets Base Path in Settings)
//   2) Add Template Folder path in Settings
//   3) Add a "file" icon button that:
//      - Copies the Template Folder to a new project folder under Base Path
//      - Finds the largest existing 8-digit prefix among TOP-LEVEL folders in Base Path
//      - Adds +1 and prompts for description
//      - Creates folder: ######## - Description
//      - Opens new folder and adds to Recent
//
// DOC TAB (restored):
// - DOC window shows a project list with controls at bottom: Add Line, Save, Status filter, Search filter
// - Open icon opens folder (supports 8-digit top-level "######## - Description" AND B00#### range folders)
// - Delete icon removes row
// - Inline edit for Job / Description / Scope / Due / Status
// - Due accepts ddmmyyyy (8 digits) or dd.mm.yyyy; stored as yyyy-mm-dd in TSV
// - Data persists to %APPDATA%\ProjectOpener\project_list.tsv
//
// Build notes:
// - NOMINMAX used; always use std::min/std::max


#define NOMINMAX


#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <winhttp.h>

#include <gdiplus.h>
#include <objidl.h>
#include "resource.h"
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <ctime>

#pragma comment(lib, "gdiplus.lib")

static const wchar_t* kDonateUrl =
L"https://www.paypal.com/donate/?hosted_button_id=2KVRDRBB8R99N";

static void EnsureGdiPlusStarted()
{
    static bool started = false;
    static ULONG_PTR token = 0;
    if (started) return;

    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok) {
        started = true;
    }
}

static HBITMAP LoadPngResourceAsHBITMAP(int resId, int* outW, int* outH)
{
    EnsureGdiPlusStarted();

    HMODULE hMod = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(resId), L"PNG");
    if (!hRes) return nullptr;

    HGLOBAL hData = LoadResource(hMod, hRes);
    if (!hData) return nullptr;

    DWORD size = SizeofResource(hMod, hRes);
    void* pBytes = LockResource(hData);
    if (!pBytes || size == 0) return nullptr;

    HGLOBAL hCopy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hCopy) return nullptr;

    void* pCopy = GlobalLock(hCopy);
    memcpy(pCopy, pBytes, size);
    GlobalUnlock(hCopy);

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(hCopy, TRUE, &stream) != S_OK) {
        GlobalFree(hCopy);
        return nullptr;
    }

    Gdiplus::Bitmap bmp(stream);
    stream->Release();

    if (bmp.GetLastStatus() != Gdiplus::Ok) return nullptr;

    if (outW) *outW = (int)bmp.GetWidth();
    if (outH) *outH = (int)bmp.GetHeight();

    HBITMAP outBmp = nullptr;
    bmp.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &outBmp);
    return outBmp;
}

static void LayoutDonateButton(HWND hwnd, HWND btnDonate, int donateW, int donateH)
{
    if (!btnDonate) return;

    RECT rc{};
    GetClientRect(hwnd, &rc);

    const int pad = 12;

    SetWindowPos(btnDonate, nullptr,
        pad,
        rc.bottom - pad - donateH,
        donateW, donateH,
        SWP_NOZORDER | SWP_NOACTIVATE);
}


#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Winhttp.lib")

#pragma comment(lib, "Gdiplus.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")


// --- Forward declarations ---
static bool PromptText(HWND owner, const wchar_t* title, const wchar_t* label, std::wstring& outText);
static bool PromptTextMultiline(HWND owner, const wchar_t* title, const wchar_t* label, std::wstring& outText);
static void CheckForUpdates_Ask(HWND owner, bool silentIfUpToDate);

static bool ProjectCrmSearchDialog(HWND owner);
static bool WriteUtf8BomFile(const std::wstring& path, const std::wstring& w);
static bool ReadUtf8CsvLines(const std::wstring& path, std::vector<std::wstring>& outLines);




#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER (ECM_FIRST + 1)
#endif


#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif


// -------------------- Defaults --------------------


// Removed default path (user must set Base Path in Settings)
static const wchar_t* kDefaultBasePath = L"";


static const wchar_t* kMainClassName = L"ProjectToolbarOpenerWnd";
static const wchar_t* kSettingsClass = L"ProjectToolbarSettingsWnd";
static const wchar_t* kDocListClass = L"ProjectToolbarDocListWnd";
static const wchar_t* kTitle = L"Project Opener";

#define WM_APP_OPEN_SETTINGS (WM_APP + 502)

// -------------------- Version / Update --------------------
#define APP_NAME      L"Project Finder"
#define APP_VERSION   L"1.2.0"
#define APP_COPYRIGHT L"© 2026 PAH. All rights reserved."

// Build / Support
#define APP_SUPPORT_EMAIL L"info.pah@mail.com"
#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)
#define APP_BUILD_DATE WIDEN(__DATE__)
#define APP_BUILD_TIME WIDEN(__TIME__)

// GitHub update endpoints
static const wchar_t* kUpdateLatestVersionUrl = L"https://raw.githubusercontent.com/pmirzapanah-hub/PAH/main/latest.txt";
static const wchar_t* kUpdateDownloadUrl = L"https://github.com/pmirzapanah-hub/PAH/releases/latest/download/PAH_Setup.exe";
static const wchar_t* kUpdateReleasesPageUrl = L"https://github.com/pmirzapanah-hub/PAH/releases";


// -------------------- Update helpers --------------------
struct SemVer { int a = 0, b = 0, c = 0; };

static std::wstring TrimWS(std::wstring s)
{
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r' || s.front() == L'\n')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return s;
}

static bool ParseSemVer(const std::wstring& s, SemVer& out)
{
    out = {};
    return swscanf_s(s.c_str(), L"%d.%d.%d", &out.a, &out.b, &out.c) == 3;
}

static int CmpSemVer(const SemVer& x, const SemVer& y)
{
    if (x.a != y.a) return x.a < y.a ? -1 : 1;
    if (x.b != y.b) return x.b < y.b ? -1 : 1;
    if (x.c != y.c) return x.c < y.c ? -1 : 1;
    return 0;
}

static bool HttpGetToWString(const std::wstring& url, std::wstring& out)
{
    out.clear();

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);

    wchar_t host[256]{};
    wchar_t path[2048]{};
    uc.lpszHostName = host; uc.dwHostNameLength = (DWORD)_countof(host);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = (DWORD)_countof(path);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    HINTERNET hSession = WinHttpOpen(L"ProjectFinder/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    bool ok = false;

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr))
    {
        std::string bytes;
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0)
        {
            std::string chunk;
            chunk.resize(avail);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), avail, &read) || read == 0) break;
            chunk.resize(read);
            bytes += chunk;
        }

        if (!bytes.empty())
        {
            int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
            if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
            if (n > 0)
            {
                out.resize(n);
                MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &out[0], n);
                out = TrimWS(out);
                ok = true;
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

static void CheckForUpdates_Ask(HWND owner, bool silentIfUpToDate)
{
    std::wstring latest;
    if (!HttpGetToWString(kUpdateLatestVersionUrl, latest))
    {
        if (!silentIfUpToDate)
            MessageBoxW(owner, L"Could not check for updates.", L"Update", MB_OK | MB_ICONWARNING);
        return;
    }

    SemVer cur{}, lat{};
    if (!ParseSemVer(APP_VERSION, cur) || !ParseSemVer(latest, lat))
    {
        if (!silentIfUpToDate)
            MessageBoxW(owner, L"Update information is invalid.", L"Update", MB_OK | MB_ICONWARNING);
        return;
    }

    if (CmpSemVer(cur, lat) >= 0)
    {
        if (!silentIfUpToDate)
            MessageBoxW(owner, L"You are up to date.", L"Update", MB_OK);
        return;
    }

    std::wstring msg = L"New version available: " + latest + L"\r\n\r\n"
        L"To update, the program will close and the installer will open.\r\n\r\nUpdate now?";
    if (MessageBoxW(owner, msg.c_str(), L"Update", MB_YESNO | MB_ICONQUESTION) == IDYES)
    {
        ShellExecuteW(nullptr, L"open", kUpdateDownloadUrl, nullptr, nullptr, SW_SHOWNORMAL);

        // Close the main window if possible (safe: posts WM_CLOSE)
        if (IsWindow(owner)) PostMessageW(owner, WM_CLOSE, 0, 0);
    }
}



// -------------------- Internal Hooks (future-proof) --------------------
struct ProjectEventInfo {
    std::wstring projectFolder;
    std::wstring projectNumber;
    std::wstring ownerName;
    std::wstring contractorCompany;
};

static void Notify_OnProjectCreated(const ProjectEventInfo&) {}
static void Notify_OnProjectOpened(const ProjectEventInfo&) {}
static void Notify_OnProjectDeleted(const ProjectEventInfo&) {}
static void Notify_OnProjectRestored(const ProjectEventInfo&) {}
static void Notify_OnSettingsSaved() {}
static void Notify_OnUpdateAccepted(const std::wstring& newVersion) {}




#define ID_ABOUT_UPDATE  50001
#define ID_ABOUT_OLD     50002
#define ID_ABOUT_EMAIL   50003

static const wchar_t ABOUT_TEXT[] =
APP_NAME L"\r\n"
L"Version: " APP_VERSION L"\r\n"
L"Build: " APP_BUILD_DATE L"\r\n"
L"\r\n"
L"Project Finder is a lightweight Windows application designed to organise project folders and CRM information in one place.\r\n"
L"\r\n"
L"Key Features\r\n"
L"\r\n"
L"- Folder-Based Project Search\r\n"
L"  Quickly locate projects by searching folder names only.\r\n"
L"  You can type a project number, client name, address, or any part of the folder name to find matching projects.\r\n"
L"\r\n"
L"- Contractor-First Project Creation\r\n"
L"  New projects begin by selecting a contractor from the Contractor Contact List.\r\n"
L"  If a contractor does not exist, a new contractor can be created and reused for future projects.\r\n"
L"\r\n"
L"- Project-Specific CRM Records\r\n"
L"  Each project stores its own \"Project Detail CRM.csv\" inside the project's Documents folder.\r\n"
L"  A combined CRM file is also maintained automatically for overview and reporting purposes.\r\n"
L"\r\n"
L"- Unified Project List\r\n"
L"  View all projects in a single list with colour-coded status rows, quick access to project folders, and safe project removal.\r\n"
L"\r\n"
L"- Email Integration\r\n"
L"  Open your default email application directly from a project to contact clients quickly.\r\n"
L"\r\n"
L"- Persistent Settings\r\n"
L"  All folder paths, preferences, and options are saved automatically and restored on startup.\r\n"
L"\r\n"
L"Check for updates: Use the button below.\r\n"
L"\r\n"
APP_COPYRIGHT L"\r\n";



// -------------------- IDs --------------------


static const int ID_EDIT = 101;


// Main toolbar buttons
static const int ID_BTN_FLAG = 200;   // Recent
static const int ID_BTN_FOLDER = 202;   // Folder dropdown
static const int ID_BTN_DOC = 203;   // DOC (Project List)
static const int ID_BTN_CRM = 204;   // CRM
static const int ID_BTN_MAIL = 205;   // (unconfirmed)
static const int ID_BTN_CAL = 206;   // Calendar
static const int ID_BTN_PHONE = 207;   // (unconfirmed)
static const int ID_BTN_GEAR = 208;   // Settings


// NEW: Template->New Project button (file icon)
static const int ID_BTN_NEWPROJECT = 209;
static const int ID_BTN_QUIT = 210;






static const int ID_BTN_MIN = 211;
// Settings controls
static const int ID_SET_EDIT_BASE = 301;
static const int ID_SET_OK = 302;
static const int ID_SET_CANCEL = 303;




static const int ID_SET_ABOUT = 304;
// Template folder controls in settings
static const int ID_SET_EDIT_TEMPLATE = 305;
static const int ID_SET_EDIT_CRM = 306;
static const int ID_SET_EDIT_CONTRACTOR = 3061;
static const int ID_SET_BROWSE_BASE = 307;
static const int ID_SET_BROWSE_TEMPLATE = 308;
static const int ID_SET_BROWSE_CRM = 309;
static const int ID_SET_BROWSE_CONTRACTOR = 3091;


static const int ID_SET_BG_BTN = 320;
static const int ID_SET_FG_BTN = 321;
static const int ID_SET_BG_SAMPLE = 322;
static const int ID_SET_FG_SAMPLE = 323;
static const int ID_SET_DONATE = 324; // Donate button


// Settings icon checkbox IDs
static const int ID_CHK_FLAG = 401;
static const int ID_CHK_FOLDER = 402;
static const int ID_CHK_DOC = 403;
static const int ID_CHK_CRM = 404;
static const int ID_CHK_MAIL = 405;
static const int ID_CHK_CAL = 406;
static const int ID_CHK_PHONE = 407;
static const int ID_CHK_GEAR = 408;


// Search mode checkbox
static const int ID_CHK_SEARCH_ANYWHERE = 409;


// Calendar context menu commands
static const int ID_CAL_OPEN = 501;
static const int ID_CAL_NEWAPPT = 502;


// Dynamic menu command bases
static const int ID_SUBFOLDER_BASE = 6000;
static const int ID_RECENT_BASE = 7000;
static const int ID_SEARCH_MATCH_BASE = 9000;


// -------------------- State --------------------


struct UiSettings
{
    // No default. Empty means "not set".
    std::wstring baseOverride;


    // NEW: Template folder path
    std::wstring templatePath;


    // CRM folder path (per company CSV). Each company (Account) becomes one CSV file in this folder.
    // Example: D:\CRM\ABC_Constructions.csv
    std::wstring crmFolderPath;


    // Contractor Contact List.csv (contractor-only list)
    std::wstring contractorCsvPath;


    bool showFlag = true;
    bool showFolder = true;
    bool showCal = true;
    bool showGear = true;


    // Confirmed: DOC visible by default
    bool showDoc = true;


    // CRM button is not used (Phone-only CRM entry point).
    bool showCRM = false;


    // unconfirmed => default OFF
    bool showMail = false;
    bool showPhone = true; // Phone is CRM entry point.


    // Search-anywhere removed per spec; always false (search folder name only)
    bool searchAnywhere = false;



    // Options
    bool useDescAsDefaultName = false; // if true, description auto-fills Owner Name by default
    bool enableSmartStatus = false;
    bool enableAdvancedSearch = false;

    // Backup frequency (days). 0 = off
    int backupEveryDays = 0;

    // Soft delete (move to .Trash instead of delete)
    bool softDeleteEnabled = true;

    COLORREF bg = RGB(0, 0, 0);
    COLORREF fg = RGB(255, 255, 255);
};


static UiSettings gSettings;


static HWND  gMainHwnd = nullptr;
static HWND  gEdit = nullptr;


static HWND  gBtnFlag = nullptr;
static HWND  gBtnFolder = nullptr;
static HWND  gBtnNewProject = nullptr;
static HWND  gBtnDoc = nullptr;
static HWND  gBtnCRM = nullptr;
static HWND  gBtnMail = nullptr;
static HWND  gBtnCal = nullptr;
static HWND  gBtnPhone = nullptr;
static HWND  gBtnGear = nullptr;
static HWND gBtnQuit = nullptr;




static HWND gBtnMin = nullptr;
static HFONT gUIFont = nullptr;
static HFONT gIconFont = nullptr;
static HFONT gMenuFont = nullptr;


static HBRUSH gBgBrush = nullptr;
static HBRUSH gMenuBgBrush = nullptr;
static HBRUSH gSelBrush = nullptr;


// White edit brush (remove black edit background)
static HBRUSH gEditWhiteBrush = nullptr;


static std::vector<std::wstring> gRecentFolders;
static std::vector<std::wstring> gLastMenuPaths;


// hover state
static HWND gHoverBtn = nullptr;
static bool gTrackingMouseLeave = false;


// owner-drawn menu label storage
static std::vector<std::wstring> gMenuText;
static int gMenuTextBase = 0;


static WNDPROC gOldEditProc = nullptr;


// -------------------- Helpers --------------------

static HBITMAP ResizeHBITMAP(HBITMAP src, int newW, int newH)
{
    if (!src) return nullptr;

    BITMAP bm{};
    GetObject(src, sizeof(bm), &bm);

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcSrc = CreateCompatibleDC(hdcScreen);
    HDC hdcDst = CreateCompatibleDC(hdcScreen);

    HBITMAP dst = CreateCompatibleBitmap(hdcScreen, newW, newH);

    HGDIOBJ oldSrc = SelectObject(hdcSrc, src);
    HGDIOBJ oldDst = SelectObject(hdcDst, dst);

    SetStretchBltMode(hdcDst, HALFTONE);
    StretchBlt(hdcDst, 0, 0, newW, newH, hdcSrc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);

    SelectObject(hdcSrc, oldSrc);
    SelectObject(hdcDst, oldDst);

    DeleteDC(hdcSrc);
    DeleteDC(hdcDst);
    ReleaseDC(nullptr, hdcScreen);

    return dst;
}



static bool DeleteDirectoryRecursive(const std::wstring& dir)
{
    // Use SHFileOperation for a reliable recursive delete (moves to recycle bin? no, this is permanent delete)
    // Build a double-null-terminated string.
    std::wstring from = dir;
    if (!from.empty() && (from.back() == L'\\' || from.back() == L'/'))
        from.pop_back();
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    int res = SHFileOperationW(&op);
    return (res == 0) && !op.fAnyOperationsAborted;
}


static void SaveUiSettings();


static std::wstring GetActiveBasePath()
{
    // Removed default fallback: user must set baseOverride in Settings.
    return gSettings.baseOverride;
}


static bool StartsWithI(const std::wstring& s, const wchar_t* prefix)
{
    size_t n = wcslen(prefix);
    if (s.size() < n) return false;
    return _wcsnicmp(s.c_str(), prefix, n) == 0;
}


static COLORREF Blend(COLORREF a, COLORREF b, int t /*0..255*/)
{
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    int rr = (ar * (255 - t) + br * t) / 255;
    int rg = (ag * (255 - t) + bg * t) / 255;
    int rb = (ab * (255 - t) + bb * t) / 255;
    return RGB(rr, rg, rb);
}


static void RebuildBrush()
{
    if (gBgBrush) DeleteObject(gBgBrush);
    gBgBrush = CreateSolidBrush(gSettings.bg);


    if (gMenuBgBrush) DeleteObject(gMenuBgBrush);
    gMenuBgBrush = CreateSolidBrush(gSettings.bg);


    if (gSelBrush) DeleteObject(gSelBrush);
    gSelBrush = CreateSolidBrush(Blend(gSettings.bg, gSettings.fg, 30));


    if (!gEditWhiteBrush) gEditWhiteBrush = CreateSolidBrush(RGB(255, 255, 255));
}


static void ApplyFont(HWND h, HFONT f)
{
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
}


static bool DirectoryExists(const std::wstring& path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}


static void OpenFolderInExplorer(const std::wstring& folderPath)
{
    std::wstring args = L"\"" + folderPath + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}


static void AddRecentFolder(const std::wstring& path)
{
    auto it = std::find_if(gRecentFolders.begin(), gRecentFolders.end(),
        [&](const std::wstring& s) { return _wcsicmp(s.c_str(), path.c_str()) == 0; });
    if (it != gRecentFolders.end()) gRecentFolders.erase(it);
    gRecentFolders.insert(gRecentFolders.begin(), path);
    if (gRecentFolders.size() > 10) gRecentFolders.resize(10);
}


static std::wstring Pad6(int n)
{
    wchar_t buf[16]{};
    swprintf_s(buf, L"%06d", n);
    return buf;
}


static std::wstring RemoveInnerSpaces(std::wstring s)
{
    s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
        }), s.end());
    return s;
}


static std::wstring TrimSpaces(std::wstring s)
{
    auto is_space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!s.empty() && is_space(s.front())) s.erase(s.begin());
    while (!s.empty() && is_space(s.back()))  s.pop_back();
    return s;
}


static std::wstring TrimAfterIdSeparators(std::wstring s)
{
    // strips any separators after ID: spaces, hyphen, underscore, dot
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'-' || s.front() == L'_' || s.front() == L'.')) {
        s.erase(s.begin());
    }
    return TrimSpaces(s);
}


static std::wstring SanitizeFolderName(std::wstring s)
{
    // Replace invalid Windows filename characters
    for (auto& ch : s) {
        switch (ch) {
        case L'\\': case L'/': case L':': case L'*': case L'?':
        case L'"': case L'<': case L'>': case L'|':
            ch = L' ';
            break;
        default: break;
        }
    }
    s = TrimSpaces(s);
    // Collapse multiple spaces
    std::wstring out;
    out.reserve(s.size());
    bool lastSpace = false;
    for (wchar_t c : s) {
        bool sp = (c == L' ' || c == L'\t');
        if (sp) {
            if (!lastSpace) out.push_back(L' ');
            lastSpace = true;
        }
        else {
            out.push_back(c);
            lastSpace = false;
        }
    }
    return TrimSpaces(out);
}


static bool ParseProjectInput(const std::wstring& raw, std::wstring& outDigits4)
{
    std::wstring s = RemoveInnerSpaces(raw);
    for (auto& ch : s) ch = (wchar_t)towupper(ch);


    if (s.size() == 7 && s[0] == L'B' && s[1] == L'0' && s[2] == L'0') {
        std::wstring d4 = s.substr(3, 4);
        if (std::all_of(d4.begin(), d4.end(), iswdigit)) { outDigits4 = d4; return true; }
        return false;
    }


    if (s.size() == 4 && std::all_of(s.begin(), s.end(), iswdigit)) { outDigits4 = s; return true; }
    return false;
}


// Normalize input to job7 like "B00####"
static bool ParseJob7FromInput(const std::wstring& raw, std::wstring& outJob7)
{
    std::wstring s = RemoveInnerSpaces(raw);
    for (auto& ch : s) ch = (wchar_t)towupper(ch);


    if (s.size() == 7 && s[0] == L'B' && s[1] == L'0' && s[2] == L'0' &&
        iswdigit(s[3]) && iswdigit(s[4]) && iswdigit(s[5]) && iswdigit(s[6])) {
        outJob7 = s;
        return true;
    }


    if (s.size() == 4 && std::all_of(s.begin(), s.end(), iswdigit)) {
        outJob7 = L"B00" + s;
        return true;
    }


    return false;
}


static bool PickColor(HWND owner, COLORREF& ioColor)
{
    static COLORREF custom[16]{};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = owner;
    cc.rgbResult = ioColor;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) {
        ioColor = cc.rgbResult;
        return true;
    }
    return false;
}


static bool BrowseForFolder(HWND owner, std::wstring& ioPath, const wchar_t* title)
{
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;


    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;


    wchar_t path[MAX_PATH]{};
    bool ok = (SHGetPathFromIDListW(pidl, path) != 0);
    CoTaskMemFree(pidl);


    if (ok) ioPath = path;
    return ok;
}


// -------------------- Search by first 7 chars --------------------


static bool FindProjectFolderByPrefix(
    const std::wstring& rangeFolderPath,
    const std::wstring& projectId7,
    std::wstring& outFullProjectPath)
{
    std::wstring pattern = rangeFolderPath + L"\\*";
    WIN32_FIND_DATAW ffd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE) return false;


    bool found = false;


    do {
        if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;


        const wchar_t* name = ffd.cFileName;
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;


        if (_wcsnicmp(name, projectId7.c_str(), 7) == 0) {
            outFullProjectPath = rangeFolderPath + L"\\" + name;
            found = true;
            break;
        }
    } while (FindNextFileW(h, &ffd));


    FindClose(h);
    return found;
}


// Resolve a project folder path from a 7-char job id like "B001234"
static bool ResolveProjectFolderFromJob7(const std::wstring& job7, std::wstring& outProjectFolderFullPath)
{
    if (job7.size() < 7) return false;
    if (!(job7[0] == L'B' && job7[1] == L'0' && job7[2] == L'0')) return false;
    if (!iswdigit(job7[3]) || !iswdigit(job7[4]) || !iswdigit(job7[5]) || !iswdigit(job7[6])) return false;


    std::wstring base = GetActiveBasePath();
    if (base.empty() || !DirectoryExists(base)) {
        MessageBoxW(gMainHwnd,
            L"Base Path is not set or not accessible.\n\nOpen Settings and set a valid folder path.",
            L"Base Path required", MB_ICONERROR);
        return false;
    }


    std::wstring digits4 = job7.substr(3, 4);
    int num = _wtoi(digits4.c_str());
    if (num < 0 || num > 9999) return false;
    if (num == 0) num = 1;


    const int blockSize = 500;
    const int start = ((num - 1) / blockSize) * blockSize + 1;
    const int end = start + (blockSize - 1);


    const std::wstring rangeFolder = L"B" + Pad6(start) + L" - " + L"B" + Pad6(end);
    std::wstring rangePath = base + L"\\" + rangeFolder;


    std::wstring foundProjectPath;
    if (FindProjectFolderByPrefix(rangePath, job7, foundProjectPath)) {
        outProjectFolderFullPath = foundProjectPath;
    }
    else {
        outProjectFolderFullPath = rangePath + L"\\" + job7;
    }
    return true;
}


static std::wstring LeafNameFromPath(const std::wstring& path)
{
    size_t last = path.find_last_of(L"\\/");
    if (last == std::wstring::npos) return path;
    return path.substr(last + 1);
}


// -------------------- Recursive search by folder name contains --------------------


static std::wstring ToLowerW(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}


static bool ContainsI(const std::wstring& hay, const std::wstring& needle)
{
    if (needle.empty()) return true;
    std::wstring h = ToLowerW(hay);
    std::wstring n = ToLowerW(needle);
    return (h.find(n) != std::wstring::npos);
}


static std::wstring RelativeUnderBase(const std::wstring& base, const std::wstring& full)
{
    if (base.empty()) return full;
    if (_wcsnicmp(full.c_str(), base.c_str(), base.size()) == 0) {
        std::wstring r = full.substr(base.size());
        if (!r.empty() && (r[0] == L'\\' || r[0] == L'/')) r.erase(r.begin());
        return r.empty() ? LeafNameFromPath(full) : r;
    }
    return full;
}


// Forward decl: used by folder-search helpers below (defined later).
static std::wstring LowerTrim(const std::wstring& s);


static void FindFoldersRecursive_Impl(
    const std::wstring& current,
    const std::wstring& query,
    std::vector<std::wstring>& out,
    int maxMatches)
{
    if ((int)out.size() >= maxMatches) return;


    std::wstring pattern = current + L"\\*";
    WIN32_FIND_DATAW ffd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE) return;


    do {
        if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;


        const wchar_t* name = ffd.cFileName;
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;


        std::wstring full = current + L"\\" + name;


        if (ContainsI(name, query)) {
            out.push_back(full);
            if ((int)out.size() >= maxMatches) { FindClose(h); return; }
        }


        FindFoldersRecursive_Impl(full, query, out, maxMatches);
        if ((int)out.size() >= maxMatches) { FindClose(h); return; }


    } while (FindNextFileW(h, &ffd));


    FindClose(h);
}


static std::vector<std::wstring> FindFoldersRecursive(const std::wstring& base, const std::wstring& query, int maxMatches = 300)
{
    std::vector<std::wstring> out;
    if (base.empty() || query.empty()) return out;
    FindFoldersRecursive_Impl(base, query, out, maxMatches);


    std::sort(out.begin(), out.end(), [&](const std::wstring& a, const std::wstring& b) {
        std::wstring ra = RelativeUnderBase(base, a);
        std::wstring rb = RelativeUnderBase(base, b);
        return _wcsicmp(ra.c_str(), rb.c_str()) < 0;
        });


    out.erase(std::unique(out.begin(), out.end(), [&](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) == 0;
        }), out.end());


    return out;
}


static std::vector<std::wstring> FindFoldersTopLevel(const std::wstring& base, const std::wstring& query, int maxMatches = 300)
{
    std::vector<std::wstring> out;
    if (base.empty() || query.empty()) return out;


    std::wstring pat = base;
    if (!pat.empty() && pat.back() != L'\\' && pat.back() != L'/') pat += L"\\";
    pat += L"*";


    WIN32_FIND_DATAW ffd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE) return out;


    std::wstring q = LowerTrim(query);


    do {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            const wchar_t* name = ffd.cFileName;
            if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;
            std::wstring n = name;
            if (LowerTrim(n).find(q) != std::wstring::npos) {
                std::wstring full = base;
                if (!full.empty() && full.back() != L'\\' && full.back() != L'/') full += L"\\";
                full += n;
                out.push_back(full);
                if ((int)out.size() >= maxMatches) break;
            }
        }
    } while (FindNextFileW(h, &ffd));


    FindClose(h);
    return out;
}




// -------------------- CRM (per-company CSV storage) --------------------


// CSV columns (exact order)
static const wchar_t* kCrmColumns[] = {
    L"LeadOwner",
    L"LeadName",
    L"Title",
    L"Account",
    L"Department",
    L"Phone",
    L"Email",
    L"CompanyAddress",
    L"CompanyWebsite",
    L"Authorization",
    L"CreatedOn",
    L"LastUpdated",
};


static std::wstring NowIsoLocal()
{
    // ISO-like: yyyy-mm-dd HH:MM:SS
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t buf[32]{};
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}


static std::wstring DigitsOnlyW(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) if (c >= L'0' && c <= L'9') out.push_back(c);
    return out;
}


static std::wstring LowerTrim(const std::wstring& s)
{
    std::wstring t = TrimSpaces(s);
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    return t;
}


static bool EnsureFolderExists(const std::wstring& folder)
{
    if (folder.empty()) return false;
    if (DirectoryExists(folder)) return true;
    if (CreateDirectoryW(folder.c_str(), nullptr)) return true;
    DWORD e = GetLastError();
    return e == ERROR_ALREADY_EXISTS;
}


static std::wstring SanitizeFileComponent(std::wstring s)
{
    // Replace invalid file name characters and collapse spaces to underscore.
    for (auto& ch : s) {
        switch (ch) {
        case L'\\': case L'/': case L':': case L'*': case L'?':
        case L'"': case L'<': case L'>': case L'|':
            ch = L' ';
            break;
        default: break;
        }
    }
    s = TrimSpaces(s);
    std::wstring out;
    out.reserve(s.size());
    bool lastUnd = false;
    for (wchar_t c : s) {
        bool sp = (c == L' ' || c == L'\t');
        if (sp) {
            if (!lastUnd) out.push_back(L'_');
            lastUnd = true;
        }
        else {
            out.push_back(c);
            lastUnd = false;
        }
    }
    out = TrimSpaces(out);
    if (out.empty()) out = L"Company";
    return out;
}


static std::wstring CrmFilePathForAccount(const std::wstring& crmFolder, const std::wstring& account)
{
    std::wstring fn = SanitizeFileComponent(account);
    return crmFolder + L"\\" + fn + L".csv";
}


struct CrmRow
{
    std::wstring LeadOwner;
    std::wstring LeadName;
    std::wstring Title;
    std::wstring Account;
    std::wstring Department;
    std::wstring Phone;
    std::wstring Email;
    std::wstring CompanyAddress;
    std::wstring CompanyWebsite;
    std::wstring Authorization;
    std::wstring CreatedOn;
    std::wstring LastUpdated;
};


static std::wstring CsvEscape(const std::wstring& v)
{
    bool needQuote = false;
    for (wchar_t c : v) {
        if (c == L',' || c == L'\"' || c == L'\r' || c == L'\n') { needQuote = true; break; }
    }
    if (!needQuote) return v;
    std::wstring out = L"\"";
    for (wchar_t c : v) {
        if (c == L'\"') out += L"\"\"";
        else out.push_back(c);
    }
    out += L"\"";
    return out;
}


static std::vector<std::wstring> CsvParseLine(const std::wstring& line)
{
    // Basic CSV parse with quotes.
    std::vector<std::wstring> cols;
    std::wstring cur;
    bool inQ = false;
    for (size_t i = 0; i < line.size(); ++i) {
        wchar_t c = line[i];
        if (inQ) {
            if (c == L'\"') {
                if (i + 1 < line.size() && line[i + 1] == L'\"') { cur.push_back(L'\"'); ++i; }
                else inQ = false;
            }
            else {
                cur.push_back(c);
            }
        }
        else {
            if (c == L',') {
                cols.push_back(cur);
                cur.clear();
            }
            else if (c == L'\"') {
                inQ = true;
            }
            else {
                cur.push_back(c);
            }
        }
    }
    cols.push_back(cur);
    return cols;
}


static std::string WideToUtf8(const std::wstring& ws)
{
    if (ws.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), out.data(), n, nullptr, nullptr);
    return out;
}


static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}


static bool ReadAllBytes(const std::wstring& path, std::string& out)
{
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return true; }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) != 0);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    return true;
}


static bool WriteAllBytes(const std::wstring& path, const std::string& data)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = (WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr) != 0);
    CloseHandle(h);
    return ok && wrote == data.size();
}


static void CrmEnsureHeaderIfMissing(const std::wstring& filePath)
{
    DWORD attrs = GetFileAttributesW(filePath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) return;


    std::wstring header;
    for (int i = 0; i < (int)_countof(kCrmColumns); ++i) {
        if (i) header += L",";
        header += kCrmColumns[i];
    }
    header += L"\r\n";


    // UTF-8 with BOM to make Excel reliably detect UTF-8.
    std::string utf8 = WideToUtf8(header);
    std::string out;
    out.push_back((char)0xEF);
    out.push_back((char)0xBB);
    out.push_back((char)0xBF);
    out += utf8;
    WriteAllBytes(filePath, out);
}


static bool CrmLoadFile(const std::wstring& filePath, std::vector<CrmRow>& rows)
{
    rows.clear();
    std::string bytes;
    if (!ReadAllBytes(filePath, bytes)) return false;
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        bytes.erase(0, 3);
    }
    std::wstring w = Utf8ToWide(bytes);
    size_t pos = 0;
    bool first = true;
    while (pos < w.size()) {
        size_t end = w.find(L"\n", pos);
        if (end == std::wstring::npos) end = w.size();
        std::wstring line = w.substr(pos, end - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        pos = end + 1;


        if (line.empty()) continue;
        if (first) { first = false; continue; } // header always present


        auto cols = CsvParseLine(line);
        while (cols.size() < _countof(kCrmColumns)) cols.push_back(L"");


        CrmRow r{};
        r.LeadOwner = cols[0];
        r.LeadName = cols[1];
        r.Title = cols[2];
        r.Account = cols[3];
        r.Department = cols[4];
        r.Phone = cols[5];
        r.Email = cols[6];
        r.CompanyAddress = cols[7];
        r.CompanyWebsite = cols[8];
        r.Authorization = cols[9];
        r.CreatedOn = cols[10];
        r.LastUpdated = cols[11];


        rows.push_back(r);
    }
    return true;
}


static bool CrmSaveFile(const std::wstring& filePath, const std::vector<CrmRow>& rows)
{
    std::wstring w;
    for (int i = 0; i < (int)_countof(kCrmColumns); ++i) {
        if (i) w += L",";
        w += kCrmColumns[i];
    }
    w += L"\r\n";


    for (const auto& r : rows) {
        const std::wstring cols[] = {
            r.LeadOwner, r.LeadName, r.Title, r.Account, r.Department,
            r.Phone, r.Email, r.CompanyAddress, r.CompanyWebsite, r.Authorization,
            r.CreatedOn, r.LastUpdated
        };
        for (int i = 0; i < (int)_countof(cols); ++i) {
            if (i) w += L",";
            w += CsvEscape(cols[i]);
        }
        w += L"\r\n";
    }


    std::string utf8 = WideToUtf8(w);
    std::string out;
    out.push_back((char)0xEF);
    out.push_back((char)0xBB);
    out.push_back((char)0xBF);
    out += utf8;
    return WriteAllBytes(filePath, out);
}


static int CrmFindMatchIndex(const std::vector<CrmRow>& rows, const CrmRow& candidate)
{
    // Priority order:
    // 1) Email (exact)
    // 2) Phone (digits-only)
    // 3) LeadName + Account (case-insensitive)


    std::wstring cEmail = TrimSpaces(candidate.Email);
    std::wstring cPhone = DigitsOnlyW(candidate.Phone);
    std::wstring cLead = LowerTrim(candidate.LeadName);
    std::wstring cAcc = LowerTrim(candidate.Account);


    if (!cEmail.empty()) {
        for (int i = 0; i < (int)rows.size(); ++i) {
            if (TrimSpaces(rows[i].Email) == cEmail) return i;
        }
    }


    if (!cPhone.empty()) {
        for (int i = 0; i < (int)rows.size(); ++i) {
            if (DigitsOnlyW(rows[i].Phone) == cPhone) return i;
        }
    }


    if (!cLead.empty() && !cAcc.empty()) {
        for (int i = 0; i < (int)rows.size(); ++i) {
            if (LowerTrim(rows[i].LeadName) == cLead && LowerTrim(rows[i].Account) == cAcc) return i;
        }
    }
    return -1;
}


// -------------------- CRM dialogs --------------------


static bool EmailLooksValid(const std::wstring& email)
{
    std::wstring e = TrimSpaces(email);
    size_t at = e.find(L'@');
    if (at == std::wstring::npos || at == 0 || at + 1 >= e.size()) return false;
    size_t dot = e.find(L'.', at + 1);
    if (dot == std::wstring::npos || dot + 1 >= e.size()) return false;
    return true;
}


struct CrmHit
{
    std::wstring filePath;
    int rowIndex = -1;
    CrmRow row;
};


static void CrmEnumerateAllHits(const std::wstring& crmFolder, std::vector<CrmHit>& out)
{
    out.clear();
    if (crmFolder.empty()) return;
    if (!EnsureFolderExists(crmFolder)) return;


    std::wstring pattern = crmFolder + L"\\*.csv";
    WIN32_FIND_DATAW ffd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE) return;


    do {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name = ffd.cFileName;
        const std::wstring fp = crmFolder + L"\\" + name;


        std::vector<CrmRow> rows;
        CrmEnsureHeaderIfMissing(fp);
        CrmLoadFile(fp, rows);
        for (int i = 0; i < (int)rows.size(); ++i) {
            CrmHit hit;
            hit.filePath = fp;
            hit.rowIndex = i;
            hit.row = rows[i];
            out.push_back(std::move(hit));
        }
    } while (FindNextFileW(h, &ffd));


    FindClose(h);
}


static std::wstring HitDisplayText(const CrmHit& h)
{
    std::wstring acc = TrimSpaces(h.row.Account);
    std::wstring lead = TrimSpaces(h.row.LeadName);
    if (acc.empty()) acc = L"(No Account)";
    if (lead.empty()) lead = L"(No Lead)";
    return acc + L" – " + lead;
}


static bool CrmSaveRowToCompanyFile(const CrmRow& rowIn)
{
    if (gSettings.crmFolderPath.empty()) return false;
    if (!EnsureFolderExists(gSettings.crmFolderPath)) return false;


    CrmRow row = rowIn;
    row.Phone = DigitsOnlyW(row.Phone);
    row.Email = TrimSpaces(row.Email);


    std::wstring fp = CrmFilePathForAccount(gSettings.crmFolderPath, row.Account);
    CrmEnsureHeaderIfMissing(fp);
    std::vector<CrmRow> rows;
    CrmLoadFile(fp, rows);


    int idx = CrmFindMatchIndex(rows, row);
    std::wstring now = NowIsoLocal();
    if (idx >= 0) {
        CrmRow& dst = rows[idx];
        // Preserve CreatedOn if present
        if (dst.CreatedOn.empty()) dst.CreatedOn = row.CreatedOn.empty() ? now : row.CreatedOn;
        dst.LastUpdated = now;


        dst.LeadOwner = row.LeadOwner;
        dst.LeadName = row.LeadName;
        dst.Title = row.Title;
        dst.Account = row.Account;
        dst.Department = row.Department;
        dst.Phone = row.Phone;
        dst.Email = row.Email;
        dst.CompanyAddress = row.CompanyAddress;
        dst.CompanyWebsite = row.CompanyWebsite;
        dst.Authorization = row.Authorization;
    }
    else {
        if (row.CreatedOn.empty()) row.CreatedOn = now;
        row.LastUpdated = now;
        rows.push_back(row);
    }


    return CrmSaveFile(fp, rows);
}


static bool CrmOpenFormDialog(HWND owner, CrmRow& ioRow, bool isNew)
{
    struct FormState {
        CrmRow row;
        bool ok = false;
        bool isNew = false;
        HWND eOwner = nullptr;
        HWND eName = nullptr;
        HWND eTitle = nullptr;
        HWND eAccount = nullptr;
        HWND eDept = nullptr;
        HWND ePhone = nullptr;
        HWND eEmail = nullptr;
        HWND eAddr = nullptr;
        HWND eWeb = nullptr;
        HWND eAuth = nullptr;
    } st;
    st.row = ioRow;
    st.isNew = isNew;


    const wchar_t* cls = L"ProjectOpenerCrmFormDlg";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l)->LRESULT {
            FormState* s = (FormState*)GetWindowLongPtrW(h, GWLP_USERDATA);
            switch (m) {
            case WM_CREATE: {
                auto* cs = (CREATESTRUCTW*)l;
                s = (FormState*)cs->lpCreateParams;
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);


                RECT rc{}; GetClientRect(h, &rc);
                int pad = 12;
                int y = pad;
                auto mkLabel = [&](const wchar_t* t, int x, int y, int w) {
                    CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE,
                        x, y, w, 18, h, nullptr, nullptr, nullptr);
                    };
                auto mkEdit = [&](HWND& he, int x, int y, int w, int hgt, DWORD style = 0) {
                    he = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | style,
                        x, y, w, hgt, h, nullptr, nullptr, nullptr);
                    SendMessageW(he, WM_SETFONT, (WPARAM)gUIFont, TRUE);
                    };


                int labelW = 110;
                int editW = rc.right - 2 * pad - labelW - 8;
                int xLabel = pad;
                int xEdit = pad + labelW + 8;
                int rowH = 24;


                mkLabel(L"Lead Owner:", xLabel, y, labelW); mkEdit(s->eOwner, xEdit, y - 2, editW, rowH); y += 28;
                mkLabel(L"Lead Name:", xLabel, y, labelW);  mkEdit(s->eName, xEdit, y - 2, editW, rowH); y += 28;
                mkLabel(L"Title:", xLabel, y, labelW);      mkEdit(s->eTitle, xEdit, y - 2, editW, rowH); y += 28;
                mkLabel(L"Account:", xLabel, y, labelW);    mkEdit(s->eAccount, xEdit, y - 2, editW, rowH); y += 28;
                mkLabel(L"Department:", xLabel, y, labelW); mkEdit(s->eDept, xEdit, y - 2, editW, rowH); y += 28;


                mkLabel(L"Phone No.:", xLabel, y, labelW);  mkEdit(s->ePhone, xEdit, y - 2, editW, rowH); y += 28;
                mkLabel(L"Email:", xLabel, y, labelW);      mkEdit(s->eEmail, xEdit, y - 2, editW, rowH); y += 28;


                mkLabel(L"Company Address:", xLabel, y, labelW);
                mkEdit(s->eAddr, xEdit, y - 2, editW, 72, ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL);
                y += 80;


                mkLabel(L"Company Website:", xLabel, y, labelW); mkEdit(s->eWeb, xEdit, y - 2, editW, rowH); y += 28;
                mkLabel(L"Authorization:", xLabel, y, labelW);   mkEdit(s->eAuth, xEdit, y - 2, editW, rowH); y += 34;


                // Populate
                SetWindowTextW(s->eOwner, s->row.LeadOwner.c_str());
                SetWindowTextW(s->eName, s->row.LeadName.c_str());
                SetWindowTextW(s->eTitle, s->row.Title.c_str());
                SetWindowTextW(s->eAccount, s->row.Account.c_str());
                SetWindowTextW(s->eDept, s->row.Department.c_str());
                SetWindowTextW(s->ePhone, s->row.Phone.c_str());
                SetWindowTextW(s->eEmail, s->row.Email.c_str());
                SetWindowTextW(s->eAddr, s->row.CompanyAddress.c_str());
                SetWindowTextW(s->eWeb, s->row.CompanyWebsite.c_str());
                SetWindowTextW(s->eAuth, s->row.Authorization.c_str());


                CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    rc.right - pad - 170, rc.bottom - pad - 30, 80, 26, h, (HMENU)1, nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    rc.right - pad - 85, rc.bottom - pad - 30, 80, 26, h, (HMENU)2, nullptr, nullptr);


                SetFocus(s->eName);
                return 0;
            }
            case WM_COMMAND: {
                int id = LOWORD(w);
                if (id == 1) {
                    auto get = [&](HWND he)->std::wstring {
                        wchar_t b[4096]{};
                        GetWindowTextW(he, b, _countof(b));
                        return TrimSpaces(b);
                        };
                    s->row.LeadOwner = get(s->eOwner);
                    s->row.LeadName = get(s->eName);
                    s->row.Title = get(s->eTitle);
                    s->row.Account = get(s->eAccount);
                    s->row.Department = get(s->eDept);
                    s->row.Phone = get(s->ePhone);
                    s->row.Email = get(s->eEmail);
                    s->row.CompanyAddress = get(s->eAddr);
                    s->row.CompanyWebsite = get(s->eWeb);
                    s->row.Authorization = get(s->eAuth);


                    std::wstring phoneDigits = DigitsOnlyW(s->row.Phone);
                    std::wstring email = TrimSpaces(s->row.Email);


                    // Phone/Email are optional per requirements.
                    if (!email.empty() && !EmailLooksValid(email)) {
                        MessageBoxW(h, L"Email does not look valid.", L"CRM", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    if (TrimSpaces(s->row.Account).empty()) {
                        MessageBoxW(h, L"Account (Company) is required.", L"CRM", MB_OK | MB_ICONWARNING);
                        return 0;
                    }


                    s->row.Phone = phoneDigits;
                    s->row.Email = email;


                    if (CrmSaveRowToCompanyFile(s->row)) {
                        s->ok = true;
                        DestroyWindow(h);
                        return 0;
                    }
                    MessageBoxW(h, L"Failed to save CRM CSV file. Check CRM Folder Path permissions.", L"CRM", MB_OK | MB_ICONERROR);
                    return 0;
                }
                if (id == 2) {
                    int r = MessageBoxW(h, L"Are you sure you want to cancel the document?", L"Cancel", MB_YESNO | MB_ICONQUESTION);
                    if (r == IDYES) {
                        DestroyWindow(h);
                    }
                    return 0;
                }
                return 0;
            }
            case WM_CLOSE:
                DestroyWindow(h);
                return 0;
            }
            return DefWindowProcW(h, m, w, l);
            };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }


    EnableWindow(owner, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls,
        isNew ? L"New Client" : L"Client Details",
        WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 560, 460,
        owner, nullptr, GetModuleHandleW(nullptr), &st);
    ShowWindow(dlg, SW_SHOW);


    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);


    if (st.ok) {
        ioRow = st.row;
        return true;
    }
    return false;
}




// Contractor Contact List.csv helpers
static std::wstring GetContractorCsvPath()
{
    std::wstring p = TrimSpaces(gSettings.contractorCsvPath);
    if (p.empty()) return L"";
    // If user picked a folder, use default file name inside it.
    if (DirectoryExists(p)) {
        if (p.back() == L'\\' || p.back() == L'/') p.pop_back();
        return p + L"\\Contractor Contact List.csv";
    }
    return p;
}


static bool EnsureContractorCsvHeader(const std::wstring& path)
{
    std::string bytes;
    if (ReadAllBytes(path, bytes)) return true;
    // Create new with header
    std::wstring header = L"Contractor Company,Contractor Name,Contractor Phone\r\n";
    return WriteUtf8BomFile(path, header);
}


static bool LoadContractors(const std::wstring& path, std::vector<CrmRow>& out)
{
    out.clear();
    std::vector<std::wstring> lines;
    if (!ReadUtf8CsvLines(path, lines)) return false;
    for (size_t i = 1; i < lines.size(); ++i) {
        auto cols = CsvParseLine(lines[i]);
        if (cols.size() < 3) continue;
        CrmRow r{};
        r.Account = cols[0];
        r.LeadName = cols[1];
        r.Phone = cols[2];
        if (!TrimSpaces(r.Account).empty()) out.push_back(r);
    }
    return true;
}


static bool AppendContractor(const std::wstring& path, const CrmRow& r)
{
    if (!EnsureContractorCsvHeader(path)) return false;
    std::string bytes;
    ReadAllBytes(path, bytes);
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) bytes.erase(0, 3);
    std::wstring w = Utf8ToWide(bytes);
    if (!w.empty() && w.back() != L'\n') w += L"\r\n";
    std::wstring line;
    line += CsvEscape(r.Account) + L"," + CsvEscape(r.LeadName) + L"," + CsvEscape(r.Phone) + L"\r\n";
    w += line;
    return WriteUtf8BomFile(path, w);


}


static bool NewContractorDialog(HWND owner, CrmRow& outRow)
{
    struct State {
        HWND eCompany = nullptr, eName = nullptr, ePhone = nullptr;
        bool ok = false;
        CrmRow row;
    } st;


    const wchar_t* cls = L"ProjectOpenerNewContractorDlg";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l)->LRESULT {
            State* s = (State*)GetWindowLongPtrW(h, GWLP_USERDATA);
            switch (m) {
            case WM_CREATE: {
                auto* cs = (CREATESTRUCTW*)l;
                s = (State*)cs->lpCreateParams;
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);


                RECT rc{}; GetClientRect(h, &rc);
                int pad = 12;
                int labelW = 160;
                int xL = pad;
                int xE = pad + labelW + 8;
                int editW = rc.right - xE - pad;
                int y = pad;
                int rowH = 24;


                auto mkLabel = [&](const wchar_t* t, int yv) {
                    HWND hl = CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE, xL, yv, labelW, 18, h, nullptr, nullptr, nullptr);
                    SendMessageW(hl, WM_SETFONT, (WPARAM)gUIFont, TRUE);
                    };
                auto mkEdit = [&](HWND& he, int yv) {
                    he = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        xE, yv - 2, editW, rowH, h, nullptr, nullptr, nullptr);
                    SendMessageW(he, WM_SETFONT, (WPARAM)gUIFont, TRUE);
                    };


                mkLabel(L"Contractor Company:", y); mkEdit(s->eCompany, y); y += 28;
                mkLabel(L"Contractor Name:", y);    mkEdit(s->eName, y);    y += 28;
                mkLabel(L"Contractor Phone:", y);   mkEdit(s->ePhone, y);   y += 34;


                HWND hOk = CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    rc.right - pad - 180, rc.bottom - pad - 28, 80, 28, h, (HMENU)ID_SET_OK, nullptr, nullptr);
                HWND hCancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    rc.right - pad - 90, rc.bottom - pad - 28, 80, 28, h, (HMENU)(INT_PTR)ID_SET_CANCEL, nullptr, nullptr);
                SendMessageW(hOk, WM_SETFONT, (WPARAM)gUIFont, TRUE);
                SendMessageW(hCancel, WM_SETFONT, (WPARAM)gUIFont, TRUE);


                SetFocus(s->eCompany);
                return 0;
            }
            case WM_COMMAND: {
                int id = LOWORD(w);
                if (id == ID_SET_CANCEL) { DestroyWindow(h); return 0; }
                if (id == ID_SET_OK) {
                    auto get = [&](HWND he) {
                        wchar_t b[512]{}; GetWindowTextW(he, b, _countof(b));
                        return TrimSpaces(b);
                        };
                    std::wstring comp = get(s->eCompany);
                    std::wstring name = get(s->eName);
                    std::wstring phone = DigitsOnlyW(get(s->ePhone));


                    if (comp.empty()) { MessageBoxW(h, L"Contractor Company is required.", L"Contractors", MB_OK | MB_ICONWARNING); SetFocus(s->eCompany); return 0; }
                    if (phone.empty()) { MessageBoxW(h, L"Contractor Phone is required.", L"Contractors", MB_OK | MB_ICONWARNING); SetFocus(s->ePhone); return 0; }


                    s->row = {};
                    s->row.Account = comp;
                    s->row.LeadName = name;
                    s->row.Phone = phone;
                    s->ok = true;
                    DestroyWindow(h);
                    return 0;
                }
                return 0;
            }
            case WM_KEYDOWN:
                if (w == VK_RETURN) {
                    SendMessageW(h, WM_COMMAND, MAKEWPARAM(ID_SET_OK, 0), 0);
                    return 0;
                }
                if (w == VK_ESCAPE) {
                    DestroyWindow(h);
                    return 0;
                }
                break;
            case WM_CLOSE:
                DestroyWindow(h);
                return 0;
            }
            return DefWindowProcW(h, m, w, l);
            };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }


    EnableWindow(owner, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"New Contractor",
        WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 210,
        owner, nullptr, GetModuleHandleW(nullptr), &st);
    ShowWindow(dlg, SW_SHOW);


    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);


    if (st.ok) { outRow = st.row; return true; }
    return false;
}




static bool CrmOpenSearchDialog(HWND owner, bool forNewProjectFlow, CrmRow* outSelected /*= nullptr*/)
{
    // Per update.pdf:
    // - This dialog is used ONLY for selecting/creating contractors during New Project (+).
    // - Phone/CRM search uses ProjectCrmSearchDialog(), not this function.
    if (!forNewProjectFlow) {
        return ProjectCrmSearchDialog(owner);
    }


    const std::wstring cpath = GetContractorCsvPath();
    if (cpath.empty()) {
        MessageBoxW(owner,
            L"Contractor Contact List.csv is not set.\n\nOpen Settings and set the Contractor Contact List.csv path.",
            L"Contractors", MB_OK | MB_ICONINFORMATION);
        return false;
    }


    // Ensure folder exists and header exists
    size_t slash = cpath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        EnsureFolderExists(cpath.substr(0, slash));
    }
    EnsureContractorCsvHeader(cpath);


    enum SearchMode { BY_CONTRACTOR_NAME = 0, BY_CONTRACTOR_COMPANY = 1, BY_CONTRACTOR_PHONE = 2 };


    struct State {
        HWND cb = nullptr;
        HWND edit = nullptr;
        HWND list = nullptr;
        bool ok = false;
        int mode = BY_CONTRACTOR_NAME;
        std::vector<CrmRow> all;
        std::vector<int> filtered;
        CrmRow selected{};
    } st;


    LoadContractors(cpath, st.all);


    const wchar_t* cls = L"ProjectOpenerContractorPickerDlg";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l)->LRESULT {
            State* s = (State*)GetWindowLongPtrW(h, GWLP_USERDATA);


            auto refresh = [&]() {
                if (!s) return;
                wchar_t qbuf[512]{};
                GetWindowTextW(s->edit, qbuf, _countof(qbuf));
                std::wstring q = TrimSpaces(qbuf);
                std::wstring qLower = LowerTrim(q);
                std::wstring qDigits = DigitsOnlyW(q);


                SendMessageW(s->list, LB_RESETCONTENT, 0, 0);
                s->filtered.clear();


                for (int i = 0; i < (int)s->all.size(); ++i) {
                    const auto& r = s->all[i];
                    bool match = false;
                    if (s->mode == BY_CONTRACTOR_NAME) {
                        match = qLower.empty() || LowerTrim(r.LeadName).find(qLower) != std::wstring::npos;
                    }
                    else if (s->mode == BY_CONTRACTOR_COMPANY) {
                        match = qLower.empty() || LowerTrim(r.Account).find(qLower) != std::wstring::npos;
                    }
                    else {
                        match = qDigits.empty() || DigitsOnlyW(r.Phone).find(qDigits) != std::wstring::npos;
                    }
                    if (match) {
                        s->filtered.push_back(i);
                        std::wstring label = r.Account + L" - " + r.LeadName;
                        if (!TrimSpaces(r.Phone).empty()) label += L" - " + r.Phone;
                        SendMessageW(s->list, LB_ADDSTRING, 0, (LPARAM)label.c_str());
                    }
                }
                };


            switch (m) {
            case WM_CREATE: {
                auto* cs = (CREATESTRUCTW*)l;
                s = (State*)cs->lpCreateParams;
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);


                RECT rc{}; GetClientRect(h, &rc);
                int pad = 12;


                CreateWindowW(L"STATIC", L"Select Contractor", WS_CHILD | WS_VISIBLE,
                    pad, pad, 250, 18, h, nullptr, nullptr, nullptr);


                CreateWindowW(L"STATIC", L"Search by:", WS_CHILD | WS_VISIBLE,
                    pad, pad + 28, 80, 18, h, nullptr, nullptr, nullptr);


                s->cb = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                    pad + 90, pad + 24, 260, 200, h, (HMENU)10, nullptr, nullptr);
                SendMessageW(s->cb, WM_SETFONT, (WPARAM)gUIFont, TRUE);


                SendMessageW(s->cb, CB_ADDSTRING, 0, (LPARAM)L"Contractor Name");
                SendMessageW(s->cb, CB_ADDSTRING, 0, (LPARAM)L"Contractor Company");
                SendMessageW(s->cb, CB_ADDSTRING, 0, (LPARAM)L"Contractor Phone");
                SendMessageW(s->cb, CB_SETCURSEL, (WPARAM)s->mode, 0);


                s->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    pad, pad + 56, rc.right - 2 * pad, 26, h, (HMENU)11, nullptr, nullptr);
                SendMessageW(s->edit, WM_SETFONT, (WPARAM)gUIFont, TRUE);


                s->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
                    pad, pad + 90, rc.right - 2 * pad, rc.bottom - (pad + 90) - 50, h, (HMENU)12, nullptr, nullptr);
                SendMessageW(s->list, WM_SETFONT, (WPARAM)gUIFont, TRUE);


                CreateWindowW(L"BUTTON", L"New Contractor", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    pad, rc.bottom - pad - 30, 130, 26, h, (HMENU)1, nullptr, nullptr);


                CreateWindowW(L"BUTTON", L"Select", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    rc.right - pad - 170, rc.bottom - pad - 30, 80, 26, h, (HMENU)2, nullptr, nullptr);


                CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    rc.right - pad - 85, rc.bottom - pad - 30, 80, 26, h, (HMENU)3, nullptr, nullptr);


                refresh();
                SetFocus(s->edit);
                return 0;
            }
            case WM_COMMAND: {
                int id = LOWORD(w);
                int code = HIWORD(w);


                if (id == 10 && code == CBN_SELCHANGE) {
                    s->mode = (int)SendMessageW(s->cb, CB_GETCURSEL, 0, 0);
                    refresh();
                    return 0;
                }
                if (id == 11 && code == EN_CHANGE) {
                    refresh();
                    return 0;
                }
                if (id == 12 && code == LBN_DBLCLK) {
                    SendMessageW(h, WM_COMMAND, MAKEWPARAM(2, 0), 0);
                    return 0;
                }


                if (id == 1) {
                    // New Contractor (Company / Name / Phone only)
                    CrmRow r{};
                    if (!NewContractorDialog(h, r)) return 0;


                    if (r.Account.empty()) {
                        MessageBoxW(h, L"Contractor Company is required.", L"Contractors", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    if (r.Phone.empty()) {
                        MessageBoxW(h, L"Contractor Phone is required.", L"Contractors", MB_OK | MB_ICONWARNING);
                        return 0;
                    }


                    if (!AppendContractor(GetContractorCsvPath(), r)) {
                        MessageBoxW(h, L"Failed to save contractor. Check Contractor Contact List.csv path permissions.",
                            L"Contractors", MB_OK | MB_ICONERROR);
                        return 0;
                    }


                    // Reload and auto-select the newly added contractor
                    LoadContractors(GetContractorCsvPath(), s->all);
                    s->selected = r;
                    s->ok = true;
                    DestroyWindow(h);
                    return 0;
                }


                if (id == 2) {
                    int sel = (int)SendMessageW(s->list, LB_GETCURSEL, 0, 0);
                    if (sel < 0 || sel >= (int)s->filtered.size()) {
                        // If nothing to select, push user to create contractor.
                        MessageBoxW(h, L"No contractor selected.\n\nClick \"New Contractor\" to add one.",
                            L"Contractors", MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                    s->selected = s->all[s->filtered[sel]];
                    s->ok = true;
                    DestroyWindow(h);
                    return 0;
                }


                if (id == 3) {
                    DestroyWindow(h);
                    return 0;
                }
                return 0;
            }
            case WM_CLOSE:
                DestroyWindow(h);
                return 0;
            }
            return DefWindowProcW(h, m, w, l);
            };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }


    EnableWindow(owner, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"Select Contractor",
        WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 560, 420,
        owner, nullptr, GetModuleHandleW(nullptr), &st);
    ShowWindow(dlg, SW_SHOW);


    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }


    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);


    if (st.ok) {
        if (outSelected) *outSelected = st.selected;
        return true;
    }
    return false;
}






// -------------------- Project CRM (.csv) --------------------
// This is the per-project CRM record that matches the user's spreadsheet layout.


static std::wstring NowCreatedOn()
{
    // dd/mm/yyyy h:mm (no seconds)
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t buf[32]{};
    swprintf_s(buf, L"%02d/%02d/%04d %d:%02d",
        tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900,
        tm.tm_hour, tm.tm_min);
    return buf;
}


struct ProjectCrmRecord
{
    std::wstring ProjectNo;          // first 8 digits
    std::wstring Title;              // Mr/Ms/Mrs/Miss
    std::wstring OwnerName;
    std::wstring Phone;
    std::wstring Email;
    std::wstring SiteAddress;        // multi-line


    std::wstring ContractorCompany;
    std::wstring ContractorName;
    std::wstring ContractorPhone;


    std::wstring CreatedOn;


    std::wstring HeardAboutUs;       // Referral / Social media / Another website / Ad / Search engine / Other
    std::wstring ReferralBy;         // if Referral
    std::wstring OtherSource;        // if Other
};


// --- Project CRM forward declarations (after structs) ---
static bool ProjectCrmOpenDialog(HWND owner, const std::wstring& projectNo8, CrmRow& ioContractor, ProjectCrmRecord& outRec, const ProjectCrmRecord* existing);
static bool SaveProjectCrmOutputs(const std::wstring& projectFolder, const ProjectCrmRecord& rec);


static std::wstring SanitizeFileNameForOwner(std::wstring s)
{
    if (s.empty()) return L"Owner";
    for (auto& ch : s) {
        switch (ch) {
        case L'\\': case L'/': case L':': case L'*': case L'?':
        case L'"': case L'<': case L'>': case L'|':
            ch = L'_';
            break;
        default: break;
        }
    }
    s = TrimSpaces(s);
    if (s.empty()) s = L"Owner";
    return s;
}


static std::wstring ProjectCcvHeader()
{
    // Match the sheet layout.
    // Note: "Project No." must be the first column.
    return
        L"Project No.,Title,Owner Name,Phone,Email,Site Address,"
        L"Contractor Company,Contractor Name,Contractor Phone,"
        L"CreatedOn,Where did you hear about us,Referral By,Other Source\r\n";
}


static std::wstring ProjectCcvRow(const ProjectCrmRecord& r)
{
    const std::wstring cols[] = {
        r.ProjectNo,
        r.Title,
        r.OwnerName,
        r.Phone,
        r.Email,
        r.SiteAddress,
        r.ContractorCompany,
        r.ContractorName,
        r.ContractorPhone,
        r.CreatedOn,
        r.HeardAboutUs,
        r.ReferralBy,
        r.OtherSource
    };


    std::wstring line;
    for (int i = 0; i < (int)_countof(cols); ++i) {
        if (i) line += L",";
        line += CsvEscape(cols[i]);
    }
    line += L"\r\n";
    return line;
}


static bool WriteUtf8BomFile(const std::wstring& path, const std::wstring& w)
{
    std::string utf8 = WideToUtf8(w);
    std::string out;
    out.push_back((char)0xEF);
    out.push_back((char)0xBB);
    out.push_back((char)0xBF);
    out += utf8;
    return WriteAllBytes(path, out);
}




// Treat gSettings.crmFolderPath as the folder that stores:
//  - Project CRM.csv
//  - Combine Project CRM.csv
static std::wstring GetProjectCrmCsvPath()
{
    if (gSettings.crmFolderPath.empty()) return L"";
    std::wstring p = gSettings.crmFolderPath;
    // If user entered a file path, use it.
    std::wstring lower = LowerTrim(p);
    if (lower.size() >= 4 && lower.substr(lower.size() - 4) == L".csv") return p;
    if (p.back() == L'\\' || p.back() == L'/') p.pop_back();
    return p + L"\\Project CRM.csv";
}


static std::wstring GetCombinedProjectCrmCsvPath()
{
    if (gSettings.crmFolderPath.empty()) return L"";
    std::wstring p = gSettings.crmFolderPath;
    std::wstring lower = LowerTrim(p);
    if (lower.size() >= 4 && lower.substr(lower.size() - 4) == L".csv") {
        // If CRM path is a file, save combined next to it.
        size_t slash = p.find_last_of(L"\\/");
        std::wstring dir = (slash == std::wstring::npos) ? L"" : p.substr(0, slash);
        return dir + L"\\Combine Project CRM.csv";
    }
    if (p.back() == L'\\' || p.back() == L'/') p.pop_back();
    return p + L"\\Combine Project CRM.csv";
}


static bool ReadUtf8CsvLines(const std::wstring& path, std::vector<std::wstring>& outLines)
{
    outLines.clear();
    std::string bytes;
    if (!ReadAllBytes(path, bytes)) return false;


    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        bytes.erase(0, 3);
    }
    std::wstring w = Utf8ToWide(bytes);


    size_t pos = 0;
    while (pos < w.size()) {
        size_t end = w.find(L"\n", pos);
        if (end == std::wstring::npos) end = w.size();
        std::wstring line = w.substr(pos, end - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        pos = end + 1;
        if (line.empty()) continue;
        outLines.push_back(line);
    }
    return true;
}


static bool UpsertProjectCrmCsv(const std::wstring& path, const ProjectCrmRecord& rec)
{
    std::vector<std::wstring> lines;
    bool has = ReadUtf8CsvLines(path, lines);


    std::wstring header = ProjectCcvHeader();
    header.pop_back(); header.pop_back(); // remove \r\n for line compare
    if (!has || lines.empty()) {
        std::wstring w = ProjectCcvHeader() + ProjectCcvRow(rec);
        return WriteUtf8BomFile(path, w);
    }


    // Ensure header is first line; if not, rebuild with header.
    if (LowerTrim(lines[0]).find(L"project no.") == std::wstring::npos) {
        std::wstring w = ProjectCcvHeader();
        for (const auto& ln : lines) {
            // skip any stray header-looking lines
            if (LowerTrim(ln).find(L"project") != std::wstring::npos) continue;
            auto cols = CsvParseLine(ln);
            if (cols.empty()) continue;
            // write back as-is line to avoid data loss
            w += ln + L"\r\n";
        }
        // then upsert
        w += ProjectCcvRow(rec);
        return WriteUtf8BomFile(path, w);
    }


    bool replaced = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        auto cols = CsvParseLine(lines[i]);
        if (cols.empty()) continue;
        if (LowerTrim(cols[0]) == LowerTrim(rec.ProjectNo)) {
            lines[i] = ProjectCcvRow(rec);
            if (!lines[i].empty() && lines[i].back() == L'\n') {
                // remove trailing newline for in-memory lines
                while (!lines[i].empty() && (lines[i].back() == L'\n' || lines[i].back() == L'\r')) lines[i].pop_back();
            }
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        std::wstring ln = ProjectCcvRow(rec);
        while (!ln.empty() && (ln.back() == L'\n' || ln.back() == L'\r')) ln.pop_back();
        lines.push_back(ln);
    }


    std::wstring out;
    out.reserve(4096 + lines.size() * 256);
    // write header with correct CRLF
    out += ProjectCcvHeader();
    // If existing header line differs, we still use standard header.
    for (size_t i = 1; i < lines.size(); ++i) {
        out += lines[i];
        out += L"\r\n";
    }
    return WriteUtf8BomFile(path, out);
}


static bool LoadProjectCrmRecords(const std::wstring& path, std::vector<ProjectCrmRecord>& out)
{
    out.clear();
    std::vector<std::wstring> lines;
    if (!ReadUtf8CsvLines(path, lines)) return false;
    if (lines.size() < 2) return true;


    for (size_t i = 1; i < lines.size(); ++i) {
        auto cols = CsvParseLine(lines[i]);
        if (cols.size() < 10) continue;
        ProjectCrmRecord r{};
        r.ProjectNo = cols.size() > 0 ? cols[0] : L"";
        r.Title = cols.size() > 1 ? cols[1] : L"";
        r.OwnerName = cols.size() > 2 ? cols[2] : L"";
        r.Phone = cols.size() > 3 ? cols[3] : L"";
        r.Email = cols.size() > 4 ? cols[4] : L"";
        r.SiteAddress = cols.size() > 5 ? cols[5] : L"";
        r.ContractorCompany = cols.size() > 6 ? cols[6] : L"";
        r.ContractorName = cols.size() > 7 ? cols[7] : L"";
        r.ContractorPhone = cols.size() > 8 ? cols[8] : L"";
        r.CreatedOn = cols.size() > 9 ? cols[9] : L"";
        r.HeardAboutUs = cols.size() > 10 ? cols[10] : L"";
        r.ReferralBy = cols.size() > 11 ? cols[11] : L"";
        r.OtherSource = cols.size() > 12 ? cols[12] : L"";
        if (!TrimSpaces(r.ProjectNo).empty()) out.push_back(r);
    }
    return true;
}




static bool ProjectCrmSearchDialog(HWND owner)
{
    const std::wstring path = GetCombinedProjectCrmCsvPath();
    if (path.empty()) {
        MessageBoxW(owner, L"Project List CRM is not set.\n\nOpen Settings and set the Project List CRM path (stores Project CRM.csv).", L"CRM", MB_OK | MB_ICONINFORMATION);
        return false;
    }


    std::vector<ProjectCrmRecord> all;
    LoadProjectCrmRecords(path, all);


    struct State {
        HWND cb = nullptr;
        HWND edit = nullptr;
        HWND list = nullptr;
        bool ok = false;
        int mode = 0; // 0 Project No, 1 Client, 2 Address
        std::vector<ProjectCrmRecord> all;
        std::vector<int> filtered;
    } st;
    st.all = all;


    const wchar_t* cls = L"ProjectOpenerProjectCrmSearchDlg";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l)->LRESULT {
            State* s = (State*)GetWindowLongPtrW(h, GWLP_USERDATA);


            auto refresh = [&]() {
                if (!s) return;
                wchar_t qbuf[512]{};
                GetWindowTextW(s->edit, qbuf, _countof(qbuf));
                std::wstring q = TrimSpaces(qbuf);
                std::wstring qLower = LowerTrim(q);
                std::wstring qDigits = DigitsOnlyW(q);


                SendMessageW(s->list, LB_RESETCONTENT, 0, 0);
                s->filtered.clear();


                for (int i = 0; i < (int)s->all.size(); ++i) {
                    const auto& r = s->all[i];
                    bool match = false;
                    if (s->mode == 0) {
                        match = qDigits.empty() || DigitsOnlyW(r.ProjectNo).find(qDigits) != std::wstring::npos;
                    }
                    else if (s->mode == 1) {
                        match = qLower.empty() || LowerTrim(r.OwnerName).find(qLower) != std::wstring::npos;
                    }
                    else {
                        match = qLower.empty() || LowerTrim(r.SiteAddress).find(qLower) != std::wstring::npos;
                    }
                    if (match) {
                        s->filtered.push_back(i);
                        std::wstring label = r.ProjectNo + L" - " + r.OwnerName;
                        SendMessageW(s->list, LB_ADDSTRING, 0, (LPARAM)label.c_str());
                    }
                }
                };


            switch (m) {
            case WM_CREATE: {
                auto* cs = (CREATESTRUCTW*)l;
                s = (State*)cs->lpCreateParams;
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);


                RECT rc{}; GetClientRect(h, &rc);
                int pad = 12;


                CreateWindowW(L"STATIC", L"Search Project CRM", WS_CHILD | WS_VISIBLE,
                    pad, pad, 260, 18, h, nullptr, nullptr, nullptr);


                CreateWindowW(L"STATIC", L"Search by:", WS_CHILD | WS_VISIBLE,
                    pad, pad + 28, 80, 18, h, nullptr, nullptr, nullptr);


                s->cb = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                    pad + 90, pad + 24, 260, 200, h, (HMENU)10, nullptr, nullptr);
                SendMessageW(s->cb, WM_SETFONT, (WPARAM)gUIFont, TRUE);


                SendMessageW(s->cb, CB_ADDSTRING, 0, (LPARAM)L"Project Number");
                SendMessageW(s->cb, CB_ADDSTRING, 0, (LPARAM)L"Client Name");
                SendMessageW(s->cb, CB_ADDSTRING, 0, (LPARAM)L"Site Address");
                SendMessageW(s->cb, CB_SETCURSEL, (WPARAM)s->mode, 0);


                s->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    pad, pad + 56, rc.right - 2 * pad, 26, h, (HMENU)11, nullptr, nullptr);
                SendMessageW(s->edit, WM_SETFONT, (WPARAM)gUIFont, TRUE);


                s->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
                    pad, pad + 90, rc.right - 2 * pad, rc.bottom - (pad + 90) - 50, h, (HMENU)12, nullptr, nullptr);
                SendMessageW(s->list, WM_SETFONT, (WPARAM)gUIFont, TRUE);


                CreateWindowW(L"BUTTON", L"Open", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    rc.right - pad - 170, rc.bottom - pad - 30, 80, 26, h, (HMENU)2, nullptr, nullptr);


                CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    rc.right - pad - 85, rc.bottom - pad - 30, 80, 26, h, (HMENU)3, nullptr, nullptr);


                refresh();
                SetFocus(s->edit);
                return 0;
            }
            case WM_COMMAND: {
                int id = LOWORD(w);
                int code = HIWORD(w);


                if (id == 10 && code == CBN_SELCHANGE) {
                    s->mode = (int)SendMessageW(s->cb, CB_GETCURSEL, 0, 0);
                    refresh();
                    return 0;
                }
                if (id == 11 && code == EN_CHANGE) {
                    refresh();
                    return 0;
                }
                if (id == 12 && code == LBN_DBLCLK) {
                    SendMessageW(h, WM_COMMAND, MAKEWPARAM(2, 0), 0);
                    return 0;
                }


                if (id == 2) {
                    int sel = (int)SendMessageW(s->list, LB_GETCURSEL, 0, 0);
                    if (sel < 0 || sel >= (int)s->filtered.size()) {
                        MessageBoxW(h, L"Select a project first.", L"CRM", MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                    const ProjectCrmRecord& r = s->all[s->filtered[sel]];
                    CrmRow contractor{};
                    contractor.Account = r.ContractorCompany;
                    contractor.LeadName = r.ContractorName;
                    contractor.Phone = r.ContractorPhone;


                    ProjectCrmRecord updated{};
                    if (ProjectCrmOpenDialog(h, r.ProjectNo, contractor, updated, &r)) {
                        SaveProjectCrmOutputs(L"", updated);
                        // Reload from disk to reflect edits
                        LoadProjectCrmRecords(GetCombinedProjectCrmCsvPath(), s->all);
                        refresh();
                    }
                    return 0;
                }


                if (id == 3) {
                    DestroyWindow(h);
                    return 0;
                }


                return 0;
            }
            case WM_CLOSE:
                DestroyWindow(h);
                return 0;
            }
            return DefWindowProcW(h, m, w, l);
            };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }


    EnableWindow(owner, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"CRM Search",
        WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 420,
        owner, nullptr, GetModuleHandleW(nullptr), &st);
    ShowWindow(dlg, SW_SHOW);


    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return true;
}




static bool SaveProjectCrmAndCombined(const ProjectCrmRecord& rec)
{
    // Per latest requirements:
    // - Per-project "Project Detail CRM.csv" is stored inside the project folder under ..\Documents
    // - The central combined file ("Combine Project CRM.csv") remains the only central CRM file.
    if (gSettings.crmFolderPath.empty()) return false;
    if (!EnsureFolderExists(gSettings.crmFolderPath)) return false;


    const std::wstring combPath = GetCombinedProjectCrmCsvPath();
    if (combPath.empty()) return false;


    // Upsert (overwrite by Project No) into combined file only.
    return UpsertProjectCrmCsv(combPath, rec);
}




static bool SavePerProjectCcv(const std::wstring& projectFolder, const ProjectCrmRecord& rec)
{
    // Per-project CRM file:
    //  - saved inside the project folder under ..\Documents
    //  - fixed file name: "Project Detail CRM.csv"
    //  - contains header + exactly one row
    std::wstring docs = projectFolder + L"\\Documents";
    EnsureFolderExists(docs);


    const std::wstring fp = docs + L"\\Project Detail CMR.csv";
    std::wstring w = ProjectCcvHeader();
    w += ProjectCcvRow(rec);
    return WriteUtf8BomFile(fp, w);
}


static bool ProjectCrmOpenDialog(HWND owner, const std::wstring& projectNo8, CrmRow& ioContractor, ProjectCrmRecord& outRec, const ProjectCrmRecord* existing = nullptr)
{
    struct State {
        ProjectCrmRecord rec;
        CrmRow contractor;
        bool ok = false;


        HWND cbTitle = nullptr;
        HWND eOwner = nullptr;
        HWND ePhone = nullptr;
        HWND eEmail = nullptr;
        HWND eAddr = nullptr;


        HWND eConCompany = nullptr;
        HWND eConName = nullptr;
        HWND eConPhone = nullptr;


        HWND cbHeard = nullptr;
        HWND eReferral = nullptr;
        HWND eOther = nullptr;
        HWND lblReferral = nullptr;
        HWND lblOther = nullptr;
    } st;


    st.rec.ProjectNo = projectNo8;
    st.contractor = ioContractor;
    if (existing) {
        st.rec = *existing;
        // Ensure ProjectNo uses the current project's first 8 digits
        st.rec.ProjectNo = projectNo8;
    }
    else {
        st.rec.CreatedOn = NowCreatedOn();
    }


    const wchar_t* cls = L"ProjectOpenerProjectCrmDlg";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l)->LRESULT {
            State* s = (State*)GetWindowLongPtrW(h, GWLP_USERDATA);


            auto showHeardExtras = [&]() {
                if (!s) return;
                int sel = (int)SendMessageW(s->cbHeard, CB_GETCURSEL, 0, 0);
                wchar_t text[128]{};
                SendMessageW(s->cbHeard, CB_GETLBTEXT, sel, (LPARAM)text);
                std::wstring v = text;


                bool isReferral = (_wcsicmp(v.c_str(), L"Referral") == 0);
                bool isOther = (_wcsicmp(v.c_str(), L"Other") == 0);


                ShowWindow(s->lblReferral, isReferral ? SW_SHOW : SW_HIDE);
                ShowWindow(s->eReferral, isReferral ? SW_SHOW : SW_HIDE);


                ShowWindow(s->lblOther, isOther ? SW_SHOW : SW_HIDE);
                ShowWindow(s->eOther, isOther ? SW_SHOW : SW_HIDE);
                };


            switch (m) {
            case WM_CREATE: {
                auto* cs = (CREATESTRUCTW*)l;
                s = (State*)cs->lpCreateParams;
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);


                RECT rc{}; GetClientRect(h, &rc);
                int pad = 12;
                int y = pad;


                auto mkLabel = [&](const wchar_t* t, int x, int y, int w) {
                    HWND hl = CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE,
                        x, y, w, 18, h, nullptr, nullptr, nullptr);
                    SendMessageW(hl, WM_SETFONT, (WPARAM)gUIFont, TRUE);
                    return hl;
                    };
                auto mkEdit = [&](HWND& he, int x, int y, int w, int hgt, DWORD style = 0) {
                    he = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | style,
                        x, y, w, hgt, h, nullptr, nullptr, nullptr);
                    SendMessageW(he, WM_SETFONT, (WPARAM)gUIFont, TRUE);
                    };


                int labelW = 140;
                int editW = rc.right - 2 * pad - labelW - 8;
                int xLabel = pad;
                int xEdit = pad + labelW + 8;
                int rowH = 24;


                // Project number (read-only text)
                wchar_t pn[128]{};
                swprintf_s(pn, L"Project No.: %s", s->rec.ProjectNo.c_str());
                mkLabel(pn, xLabel, y, rc.right - 2 * pad); y += 26;


                mkLabel(L"Title:", xLabel, y, labelW);
                s->cbTitle = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                    xEdit, y - 2, editW, 200, h, (HMENU)100, nullptr, nullptr);
                SendMessageW(s->cbTitle, WM_SETFONT, (WPARAM)gUIFont, TRUE);
                SendMessageW(s->cbTitle, CB_ADDSTRING, 0, (LPARAM)L"Mr.");
                SendMessageW(s->cbTitle, CB_ADDSTRING, 0, (LPARAM)L"Ms.");
                SendMessageW(s->cbTitle, CB_ADDSTRING, 0, (LPARAM)L"Mrs.");
                SendMessageW(s->cbTitle, CB_ADDSTRING, 0, (LPARAM)L"Miss");
                SendMessageW(s->cbTitle, CB_SETCURSEL, 0, 0);
                y += 28;


                mkLabel(L"Owner Name:", xLabel, y, labelW); mkEdit(s->eOwner, xEdit, y - 2, editW, rowH); y += 28;

                // Prefill Owner Name (use existing record if provided)
                if (!s->rec.OwnerName.empty()) SetWindowTextW(s->eOwner, s->rec.OwnerName.c_str());
                mkLabel(L"Phone:", xLabel, y, labelW);      mkEdit(s->ePhone, xEdit, y - 2, editW, rowH); y += 28;
                mkLabel(L"Email:", xLabel, y, labelW);      mkEdit(s->eEmail, xEdit, y - 2, editW, rowH); y += 28;


                mkLabel(L"Site Address:", xLabel, y, labelW);
                mkEdit(s->eAddr, xEdit, y - 2, editW, 72, ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL);
                y += 80;


                // Contractor fields
                mkLabel(L"Contractor Company:", xLabel, y, labelW); mkEdit(s->eConCompany, xEdit, y - 2, editW, rowH); y += 28;
                mkLabel(L"Contractor Name:", xLabel, y, labelW);    mkEdit(s->eConName, xEdit, y - 2, editW, rowH); y += 28;
                mkLabel(L"Contractor Phone:", xLabel, y, labelW);   mkEdit(s->eConPhone, xEdit, y - 2, editW, rowH); y += 28;


                // Where did you hear about us
                mkLabel(L"Where did you hear about us:", xLabel, y, labelW);
                s->cbHeard = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                    xEdit, y - 2, editW, 240, h, (HMENU)101, nullptr, nullptr);
                SendMessageW(s->cbHeard, WM_SETFONT, (WPARAM)gUIFont, TRUE);
                const wchar_t* opts[] = { L"Search engine", L"Referral", L"Social media", L"Another website", L"Ad", L"Other" };
                for (auto* o : opts) SendMessageW(s->cbHeard, CB_ADDSTRING, 0, (LPARAM)o);
                SendMessageW(s->cbHeard, CB_SETCURSEL, 0, 0);
                y += 28;


                s->lblReferral = mkLabel(L"Referral By:", xLabel, y, labelW); mkEdit(s->eReferral, xEdit, y - 2, editW, rowH); y += 28;
                s->lblOther = mkLabel(L"Other Source:", xLabel, y, labelW);   mkEdit(s->eOther, xEdit, y - 2, editW, rowH); y += 30;




                // Pre-fill fields (edit mode if existing record provided)
                // Owner / contact
                SetWindowTextW(s->eOwner, s->rec.OwnerName.c_str());
                SetWindowTextW(s->ePhone, s->rec.Phone.c_str());
                SetWindowTextW(s->eEmail, s->rec.Email.c_str());
                SetWindowTextW(s->eAddr, s->rec.SiteAddress.c_str());


                // Title dropdown
                auto setComboText = [&](HWND cb, const std::wstring& want) {
                    if (!cb) return;
                    int n = (int)SendMessageW(cb, CB_GETCOUNT, 0, 0);
                    for (int i = 0; i < n; ++i) {
                        wchar_t t[128]{};
                        SendMessageW(cb, CB_GETLBTEXT, i, (LPARAM)t);
                        if (_wcsicmp(t, want.c_str()) == 0) {
                            SendMessageW(cb, CB_SETCURSEL, i, 0);
                            return;
                        }
                    }
                    };
                if (!s->rec.Title.empty()) setComboText(s->cbTitle, s->rec.Title);


                // Contractor (prefer record values if present)
                std::wstring cc = s->rec.ContractorCompany.empty() ? s->contractor.Account : s->rec.ContractorCompany;
                std::wstring cn = s->rec.ContractorName.empty() ? s->contractor.LeadName : s->rec.ContractorName;
                std::wstring cp = s->rec.ContractorPhone.empty() ? s->contractor.Phone : s->rec.ContractorPhone;
                SetWindowTextW(s->eConCompany, cc.c_str());
                SetWindowTextW(s->eConName, cn.c_str());
                SetWindowTextW(s->eConPhone, cp.c_str());


                // Heard-about-us dropdown
                if (!s->rec.HeardAboutUs.empty()) setComboText(s->cbHeard, s->rec.HeardAboutUs);
                SetWindowTextW(s->eReferral, s->rec.ReferralBy.c_str());
                SetWindowTextW(s->eOther, s->rec.OtherSource.c_str());
                showHeardExtras();


                // Buttons
                CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    rc.right - pad - 170, rc.bottom - pad - 30, 80, 26, h, (HMENU)1, nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    rc.right - pad - 85, rc.bottom - pad - 30, 80, 26, h, (HMENU)2, nullptr, nullptr);


                showHeardExtras();
                SetFocus(s->eOwner);
                return 0;
            }
            case WM_COMMAND: {
                int id = LOWORD(w);
                int code = HIWORD(w);


                if (id == 101 && code == CBN_SELCHANGE) {
                    showHeardExtras();
                    return 0;
                }


                if (id == 1) {
                    auto get = [&](HWND he)->std::wstring {
                        wchar_t b[4096]{};
                        GetWindowTextW(he, b, _countof(b));
                        return TrimSpaces(b);
                        };


                    wchar_t title[64]{};
                    int tsel = (int)SendMessageW(s->cbTitle, CB_GETCURSEL, 0, 0);
                    if (tsel < 0) tsel = 0;
                    SendMessageW(s->cbTitle, CB_GETLBTEXT, tsel, (LPARAM)title);
                    s->rec.Title = title;


                    s->rec.OwnerName = get(s->eOwner);
                    s->rec.Phone = DigitsOnlyW(get(s->ePhone));
                    s->rec.Email = get(s->eEmail);
                    s->rec.SiteAddress = get(s->eAddr);


                    s->rec.ContractorCompany = get(s->eConCompany);
                    s->rec.ContractorName = get(s->eConName);
                    s->rec.ContractorPhone = DigitsOnlyW(get(s->eConPhone));


                    wchar_t heard[128]{};
                    int hsel = (int)SendMessageW(s->cbHeard, CB_GETCURSEL, 0, 0);
                    if (hsel < 0) hsel = 0;
                    SendMessageW(s->cbHeard, CB_GETLBTEXT, hsel, (LPARAM)heard);
                    s->rec.HeardAboutUs = heard;


                    s->rec.ReferralBy.clear();
                    s->rec.OtherSource.clear();
                    if (_wcsicmp(s->rec.HeardAboutUs.c_str(), L"Referral") == 0) s->rec.ReferralBy = get(s->eReferral);
                    if (_wcsicmp(s->rec.HeardAboutUs.c_str(), L"Other") == 0) s->rec.OtherSource = get(s->eOther);


                    // Validation: Phone OR Email must be provided
                    if (s->rec.OwnerName.empty()) {
                        MessageBoxW(h, L"Owner Name is required.", L"CRM", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    if (!s->rec.Email.empty() && !EmailLooksValid(s->rec.Email)) {
                        MessageBoxW(h, L"Email does not look valid.", L"CRM", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    if (s->rec.ContractorCompany.empty()) {
                        MessageBoxW(h, L"Contractor Company is required.", L"CRM", MB_OK | MB_ICONWARNING);
                        return 0;
                    }


                    s->ok = true;
                    DestroyWindow(h);
                    return 0;
                }
                if (id == 2) {
                    int r = MessageBoxW(h, L"Are you sure you want to cancel the document?", L"Cancel", MB_YESNO | MB_ICONQUESTION);
                    if (r == IDYES) {
                        DestroyWindow(h);
                    }
                    return 0;
                }
                return 0;
            }
            case WM_CLOSE:
                DestroyWindow(h);
                return 0;
            }
            return DefWindowProcW(h, m, w, l);
            };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }


    EnableWindow(owner, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"Project CRM",
        WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 640,
        owner, nullptr, GetModuleHandleW(nullptr), &st);
    ShowWindow(dlg, SW_SHOW);


    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);


    if (!st.ok) return false;


    // Contractor save-as-new behavior:
    // If contractor details were changed, store a new contractor row.
    CrmRow newCon{};
    newCon.Account = st.rec.ContractorCompany;
    newCon.LeadName = st.rec.ContractorName;
    newCon.Phone = st.rec.ContractorPhone;
    newCon.Email = L""; // not used for contractor flow here
    newCon.LeadOwner = L"";
    newCon.Title = L"";
    newCon.Department = L"";
    newCon.CompanyAddress = L"";
    newCon.CompanyWebsite = L"";
    newCon.Authorization = L"";
    if (!newCon.Account.empty()) {
        // Save contractor. If edited, it will create/merge depending on existing matching rules.
        // (This keeps the existing CRM storage system usable.)
        CrmSaveRowToCompanyFile(newCon);
    }


    outRec = st.rec;
    ioContractor = newCon;
    return true;
}




static bool SaveProjectCrmOutputs(const std::wstring& projectFolder, const ProjectCrmRecord& rec)
{
    // Per latest requirements:
    //  - Per-project file: ..\Documents\Project Detail CRM.csv (header + 1 row)
    //  - Central file: Combine Project CRM.csv (unchanged upsert behavior)
    bool okCombined = SaveProjectCrmAndCombined(rec);


    bool okPerProject = true;
    if (!projectFolder.empty()) {
        okPerProject = SavePerProjectCcv(projectFolder, rec);
    }
    return okCombined && okPerProject;
}


// -------------------- Outlook actions --------------------


static void OutlookOpenCalendar()
{
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"outlookcal:", nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        ShellExecuteW(nullptr, L"open", L"outlook.exe", L"/select outlook:calendar", nullptr, SW_SHOWNORMAL);
    }
}


static void OutlookNewAppointment()
{
    ShellExecuteW(nullptr, L"open", L"outlook.exe", L"/c ipm.appointment", nullptr, SW_SHOWNORMAL);
}


// -------------------- Auto-size bar --------------------


static int CountVisibleButtons()
{
    int n = 0;
    if (gSettings.showFlag)   n++;
    if (gSettings.showFolder) n++;
    // gBtnNewProject is always shown (not part of settings)
    n++;
    n++; // Quit button (always visible)
    if (gSettings.showDoc)    n++;
    if (gSettings.showMail)   n++;
    if (gSettings.showCal)    n++;
    if (gSettings.showPhone)  n++;
    if (gSettings.showGear)   n++;
    return n;
}


static void ResizeWindowToFit()
{
    const int padL = 6;
    const int padR = 6;
    const int btnW = 30;
    const int gapBtn = 5;
    const int editW = 165;
    const int gapAfterEdit = 8;


    int width = padL;


    if (gSettings.showFlag)   width += btnW + gapBtn;
    if (gSettings.showFolder) width += btnW + gapBtn;


    width += editW + gapAfterEdit;


    // New Project button always shown after edit
    width += btnW + gapBtn;


    if (gSettings.showDoc)   width += btnW + gapBtn;
    if (gSettings.showMail)  width += btnW + gapBtn;
    if (gSettings.showCal)   width += btnW + gapBtn;
    if (gSettings.showPhone) width += btnW + gapBtn;
    if (gSettings.showGear)  width += btnW + gapBtn;
    width += btnW + gapBtn; // Quit


    if (CountVisibleButtons() > 0) width -= gapBtn;
    width += padR;


    const int height = 40;
    SetWindowPos(gMainHwnd, nullptr, 0, 0, width, height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}


static void LayoutMainBar()
{
    if (!gMainHwnd) return;


    RECT rc{}; GetClientRect(gMainHwnd, &rc);
    const int W = rc.right - rc.left;


    const int pad = 6;
    const int btnW = 30;
    const int btnH = 24;


    // Two-row layout:
    // Row 1 (top): Search field (resizes) + Minimize/Close on right
    // Row 2 (bottom): all action buttons
    const int yTop = pad;
    const int yBottom = yTop + btnH + pad;


    // Right-side window buttons
    int xRight = W - pad;


    auto placeRight = [&](HWND hBtn, bool visible)
        {
            if (!hBtn) return;
            ShowWindow(hBtn, visible ? SW_SHOW : SW_HIDE);
            if (!visible) return;
            xRight -= btnW;
            SetWindowPos(hBtn, nullptr, xRight, yTop, btnW, btnH, SWP_NOZORDER);
            xRight -= 5;
        };


    // Close and Minimize (top row)
    placeRight(gBtnQuit, true);
    placeRight(gBtnMin, true);


    // Search edit (top row)
    if (gEdit) {
        int editX = pad;
        int editW = (xRight - pad) - editX; // xRight already includes a trailing gap
        if (editW < 120) editW = 120;
        SetWindowPos(gEdit, nullptr, editX, yTop, editW, btnH, SWP_NOZORDER);
        ShowWindow(gEdit, SW_SHOW);
    }


    // Row 2 buttons (left to right)
    int x = pad;


    auto placeBtn = [&](HWND hBtn, bool visible)
        {
            if (!hBtn) return;
            ShowWindow(hBtn, visible ? SW_SHOW : SW_HIDE);
            if (visible) {
                SetWindowPos(hBtn, nullptr, x, yBottom, btnW, btnH, SWP_NOZORDER);
                x += btnW + 5;
            }
        };


    placeBtn(gBtnFlag, gSettings.showFlag);
    placeBtn(gBtnFolder, gSettings.showFolder);
    placeBtn(gBtnNewProject, true);
    placeBtn(gBtnDoc, gSettings.showDoc);
    placeBtn(gBtnMail, gSettings.showMail);
    placeBtn(gBtnCal, gSettings.showCal);
    placeBtn(gBtnPhone, gSettings.showPhone);
    placeBtn(gBtnGear, gSettings.showGear);


    // Keep the window height fixed to two rows; width is user-resizable.
    const int desiredH = yBottom + btnH + pad;
    RECT wr{}; GetWindowRect(gMainHwnd, &wr);
    int curW = wr.right - wr.left;
    int curH = wr.bottom - wr.top;
    if (curH != desiredH) {
        SetWindowPos(gMainHwnd, nullptr, 0, 0, curW, desiredH, SWP_NOMOVE | SWP_NOZORDER);
    }


    InvalidateRect(gMainHwnd, nullptr, TRUE);
}




// -------------------- Themed popup helpers --------------------


static void MenuTheme_Begin(int baseId, int count)
{
    gMenuTextBase = baseId;
    gMenuText.assign(count, L"");
}


static void MenuTheme_SetText(int cmdId, const std::wstring& s)
{
    int idx = cmdId - gMenuTextBase;
    if (idx >= 0 && idx < (int)gMenuText.size()) gMenuText[idx] = s;
}


static const std::wstring* MenuTheme_GetText(int cmdId)
{
    int idx = cmdId - gMenuTextBase;
    if (idx >= 0 && idx < (int)gMenuText.size()) return &gMenuText[idx];
    return nullptr;
}


static void MenuTheme_AppendOwnerDrawItem(HMENU menu, int cmdId, const std::wstring& label)
{
    MENUITEMINFOW mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_ID | MIIM_FTYPE;
    mi.wID = cmdId;
    mi.fType = MFT_OWNERDRAW;
    InsertMenuItemW(menu, (UINT)-1, TRUE, &mi);
    MenuTheme_SetText(cmdId, label);
}


static int ShowThemedPopupMenuAtPoint(HMENU menu, POINT ptScreen)
{
    SetForegroundWindow(gMainHwnd);


    int cmd = TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        ptScreen.x, ptScreen.y, 0, gMainHwnd, nullptr);


    DestroyMenu(menu);
    return cmd;
}


static int ShowThemedPopupMenu(HMENU menu, HWND anchorBtn)
{
    RECT r{};
    GetWindowRect(anchorBtn, &r);
    POINT pt{ r.left, r.bottom };
    return ShowThemedPopupMenuAtPoint(menu, pt);
}


// -------------------- Owner-draw renderers --------------------


static void DrawFancyIconTile(HDC hdc, const RECT& rc, const wchar_t* text,
    HFONT font, bool hot, bool pressed, bool disabled, bool focused)
{
    FillRect(hdc, &rc, gBgBrush);


    RECT tile = rc;
    InflateRect(&tile, -2, -2);


    COLORREF border = Blend(gSettings.fg, gSettings.bg, 120);
    COLORREF fillNormal = gSettings.bg;
    COLORREF fillHot = Blend(gSettings.bg, gSettings.fg, 28);
    COLORREF fillPressed = Blend(gSettings.bg, gSettings.fg, 45);


    COLORREF fill = fillNormal;
    if (pressed) fill = fillPressed;
    else if (hot) fill = fillHot;


    HBRUSH hFill = CreateSolidBrush(fill);
    FillRect(hdc, &tile, hFill);
    DeleteObject(hFill);


    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, tile.left, tile.top, tile.right, tile.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);


    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? Blend(gSettings.fg, gSettings.bg, 160) : gSettings.fg);
    HFONT oldF = (HFONT)SelectObject(hdc, font);


    RECT tr = tile;
    if (pressed) OffsetRect(&tr, 1, 1);
    DrawTextW(hdc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);


    SelectObject(hdc, oldF);


    if (focused) {
        RECT fr = tile;
        InflateRect(&fr, -3, -3);
        DrawFocusRect(hdc, &fr);
    }
}


static void DrawFancyIconButton(const DRAWITEMSTRUCT* dis)
{
    wchar_t text[64]{};
    GetWindowTextW(dis->hwndItem, text, _countof(text));


    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;
    const bool hot = (gHoverBtn == dis->hwndItem);


    DrawFancyIconTile(dis->hDC, dis->rcItem, text, gIconFont, hot, pressed, disabled, focused);
}


static void MeasureThemedMenuItem(MEASUREITEMSTRUCT* mi)
{
    const std::wstring* label = MenuTheme_GetText((int)mi->itemID);
    std::wstring text = label ? *label : L"";


    HDC hdc = GetDC(gMainHwnd);
    HFONT oldF = (HFONT)SelectObject(hdc, gMenuFont ? gMenuFont : gUIFont);


    SIZE sz{};
    GetTextExtentPoint32W(hdc, text.c_str(), (int)text.size(), &sz);


    SelectObject(hdc, oldF);
    ReleaseDC(gMainHwnd, hdc);


    mi->itemHeight = (UINT)std::max(24, (int)sz.cy + 10);
    mi->itemWidth = (UINT)sz.cx + 30;
}


static void DrawThemedMenuItem(const DRAWITEMSTRUCT* dis)
{
    RECT rc = dis->rcItem;
    HDC hdc = dis->hDC;


    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;


    FillRect(hdc, &rc, selected ? gSelBrush : gMenuBgBrush);


    const std::wstring* label = MenuTheme_GetText((int)dis->itemID);
    std::wstring text = label ? *label : L"";


    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? Blend(gSettings.fg, gSettings.bg, 160) : gSettings.fg);


    HFONT oldF = (HFONT)SelectObject(hdc, gMenuFont ? gMenuFont : gUIFont);


    RECT tr = rc;
    tr.left += 10;
    tr.right -= 10;
    DrawTextW(hdc, text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);


    SelectObject(hdc, oldF);
}


// -------------------- Mouse tracking --------------------


static void BeginTrackMouseLeave()
{
    if (gTrackingMouseLeave) return;
    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = gMainHwnd;
    TrackMouseEvent(&tme);
    gTrackingMouseLeave = true;
}


// -------------------- Main edit: apply mode --------------------


static void ApplySearchModeToEdit()
{
    if (!gEdit) return;


    if (gSettings.searchAnywhere) {
        SendMessageW(gEdit, EM_SETLIMITTEXT, 200, 0);
        SendMessageW(gEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search folder name...");
    }
    else {
        SendMessageW(gEdit, EM_SETLIMITTEXT, 7, 0);
        SendMessageW(gEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"B00XXXX");
    }
}


// -------------------- Folder listing --------------------


static std::vector<std::wstring> ListSubfolders(const std::wstring& parent)
{
    std::vector<std::wstring> out;


    std::wstring pattern = parent + L"\\*";
    WIN32_FIND_DATAW ffd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE) return out;


    do {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            const wchar_t* name = ffd.cFileName;
            if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;
            out.emplace_back(name);
        }
    } while (FindNextFileW(h, &ffd));


    FindClose(h);


    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });


    return out;
}


static std::wstring ShortLabelFromPath(const std::wstring& path)
{
    size_t last = path.find_last_of(L"\\/");
    if (last == std::wstring::npos) return path;
    size_t prev = path.find_last_of(L"\\/", (last > 0) ? last - 1 : 0);
    if (prev == std::wstring::npos) return path.substr(last + 1);
    return path.substr(prev + 1);
}


// -------------------- Dropdowns --------------------


static void ShowFolderDropdown();
static void ShowRecentDropdown();


// show matches dropdown
static void ShowSearchMatchesDropdown(const std::vector<std::wstring>& matches)
{
    if (matches.empty()) return;


    HMENU menu = CreatePopupMenu();
    gLastMenuPaths.clear();
    gLastMenuPaths.reserve(matches.size());


    const int maxItems = (int)std::min<size_t>(300, matches.size());
    MenuTheme_Begin(ID_SEARCH_MATCH_BASE, maxItems);


    std::wstring base = GetActiveBasePath();


    for (int i = 0; i < maxItems; ++i) {
        gLastMenuPaths.push_back(matches[i]);
        std::wstring label = RelativeUnderBase(base, matches[i]);
        MenuTheme_AppendOwnerDrawItem(menu, ID_SEARCH_MATCH_BASE + i, label);
    }


    RECT er{};
    GetWindowRect(gEdit, &er);
    POINT pt{ er.left, er.bottom };


    int cmd = ShowThemedPopupMenuAtPoint(menu, pt);


    if (cmd >= ID_SEARCH_MATCH_BASE && cmd < ID_SEARCH_MATCH_BASE + (int)gLastMenuPaths.size()) {
        const std::wstring& chosen = gLastMenuPaths[cmd - ID_SEARCH_MATCH_BASE];
        if (DirectoryExists(chosen)) {
            OpenFolderInExplorer(chosen);
            AddRecentFolder(chosen);
        }
        else {
            MessageBoxW(gMainHwnd, L"Selected folder no longer exists.", L"Search", MB_ICONERROR);
        }
    }
}


// -------------------- Project open logic --------------------


static bool ResolveProjectFolderFromEdit(std::wstring& outProjectFolderFullPath)
{
    wchar_t buf[512]{};
    GetWindowTextW(gEdit, buf, (int)_countof(buf));
    std::wstring raw = TrimSpaces(buf);


    if (raw.empty()) {
        MessageBoxW(gMainHwnd, L"Type something to search for.", L"Search", MB_ICONINFORMATION);
        return false;
    }


    std::wstring base = GetActiveBasePath();
    if (base.empty() || !DirectoryExists(base)) {
        MessageBoxW(gMainHwnd,
            L"Base Path is not set or not accessible.\n\nOpen Settings and set a valid folder path.",
            L"Search", MB_ICONERROR);
        return false;
    }


    // Folder-name only search (top-level folders under Base Path).
    auto matches = FindFoldersTopLevel(base, raw, 300);


    if (matches.empty()) {
        std::wstring msg = L"No folders found matching:\n\n" + raw;
        MessageBoxW(gMainHwnd, msg.c_str(), L"Search", MB_ICONINFORMATION);
        return false;
    }


    if (matches.size() == 1) {
        outProjectFolderFullPath = matches[0];
        return true;
    }


    // More than one match: show options so the user can pick.
    ShowSearchMatchesDropdown(matches);
    return false;
}




static void OpenProjectFromEnter()
{
    std::wstring projectPath;
    if (!ResolveProjectFolderFromEdit(projectPath)) { SetFocus(gEdit); return; }


    if (!DirectoryExists(projectPath)) {
        std::wstring msg = L"Project folder not found:\n\n" + projectPath;
        MessageBoxW(gMainHwnd, msg.c_str(), L"Not found", MB_ICONERROR);
        SetFocus(gEdit);
        return;
    }


    OpenFolderInExplorer(projectPath);
    AddRecentFolder(projectPath);


    SetWindowTextW(gEdit, L"");
    SetFocus(gEdit);
}


static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN)
    {
        if (wParam == VK_RETURN) { OpenProjectFromEnter(); return 0; }


        if (wParam == VK_F4 || (wParam == VK_DOWN && (GetKeyState(VK_MENU) & 0x8000))) {
            ShowFolderDropdown();
            return 0;
        }


        if (wParam == VK_F3 || (wParam == VK_UP && (GetKeyState(VK_MENU) & 0x8000))) {
            ShowRecentDropdown();
            return 0;
        }


        if (wParam == VK_ESCAPE) {
            SetWindowTextW(gEdit, L"");
            return 0;
        }
    }


    return CallWindowProcW(gOldEditProc, hwnd, msg, wParam, lParam);
}


// -------------------- Folder dropdown + Recent dropdown --------------------


static void ShowFolderDropdown()
{
    std::wstring projectPath;


    if (!ResolveProjectFolderFromEdit(projectPath)) { SetFocus(gEdit); return; }


    if (!DirectoryExists(projectPath)) {
        std::wstring msg = L"Folder not found:\n\n" + projectPath;
        MessageBoxW(gMainHwnd, msg.c_str(), L"Folder", MB_ICONERROR);
        SetFocus(gEdit);
        return;
    }


    auto subfolders = ListSubfolders(projectPath);
    if (subfolders.empty()) {
        MessageBoxW(gMainHwnd, L"No subfolders found.", L"Folder", MB_ICONINFORMATION);
        return;
    }


    HMENU menu = CreatePopupMenu();
    gLastMenuPaths.clear();
    gLastMenuPaths.reserve(subfolders.size());


    const int maxItems = (int)std::min((size_t)300, subfolders.size());
    MenuTheme_Begin(ID_SUBFOLDER_BASE, maxItems);


    for (int i = 0; i < maxItems; ++i) {
        std::wstring full = projectPath + L"\\" + subfolders[i];
        gLastMenuPaths.push_back(full);
        MenuTheme_AppendOwnerDrawItem(menu, ID_SUBFOLDER_BASE + i, subfolders[i]);
    }


    int cmd = ShowThemedPopupMenu(menu, gBtnFolder);


    if (cmd >= ID_SUBFOLDER_BASE && cmd < ID_SUBFOLDER_BASE + (int)gLastMenuPaths.size()) {
        const std::wstring& chosen = gLastMenuPaths[cmd - ID_SUBFOLDER_BASE];
        if (DirectoryExists(chosen)) {
            OpenFolderInExplorer(chosen);
            AddRecentFolder(chosen);
        }
        else {
            MessageBoxW(gMainHwnd, L"Selected folder no longer exists.", L"Folder", MB_ICONERROR);
        }
    }
}


static void ShowRecentDropdown()
{
    if (gRecentFolders.empty()) {
        MessageBoxW(gMainHwnd, L"No recent folders yet.\n\nOpen a project or subfolder first.",
            L"Recent", MB_ICONINFORMATION);
        return;
    }


    HMENU menu = CreatePopupMenu();
    gLastMenuPaths.clear();
    gLastMenuPaths.reserve(gRecentFolders.size());


    const int count = (int)gRecentFolders.size();
    MenuTheme_Begin(ID_RECENT_BASE, count);


    for (int i = 0; i < count; ++i) {
        gLastMenuPaths.push_back(gRecentFolders[i]);
        MenuTheme_AppendOwnerDrawItem(menu, ID_RECENT_BASE + i, ShortLabelFromPath(gRecentFolders[i]));
    }


    int cmd = ShowThemedPopupMenu(menu, gBtnFlag);


    if (cmd >= ID_RECENT_BASE && cmd < ID_RECENT_BASE + (int)gLastMenuPaths.size()) {
        const std::wstring& chosen = gLastMenuPaths[cmd - ID_RECENT_BASE];
        if (DirectoryExists(chosen)) {
            OpenFolderInExplorer(chosen);
            AddRecentFolder(chosen);
        }
        else {
            MessageBoxW(gMainHwnd, L"That folder no longer exists.", L"Recent", MB_ICONERROR);
        }
    }
}


// -------------------- Calendar menu --------------------


static void ShowCalendarContextMenu(HWND hwnd, POINT ptScreen)
{
    HMENU menu = CreatePopupMenu();


    MenuTheme_Begin(ID_CAL_OPEN, 2);
    MenuTheme_AppendOwnerDrawItem(menu, ID_CAL_OPEN, L"Open Calendar");
    MenuTheme_AppendOwnerDrawItem(menu, ID_CAL_NEWAPPT, L"New Appointment");


    SetForegroundWindow(gMainHwnd);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        ptScreen.x, ptScreen.y, 0, hwnd, nullptr);


    DestroyMenu(menu);


    if (cmd == ID_CAL_OPEN) OutlookOpenCalendar();
    if (cmd == ID_CAL_NEWAPPT) OutlookNewAppointment();
}


// -------------------- NEW: Template copy + New project creation --------------------


static bool TryParse8DigitPrefix(const std::wstring& name, int& outNum)
{
    if (name.size() < 8) return false;
    for (int i = 0; i < 8; ++i) if (!iswdigit(name[i])) return false;
    outNum = _wtoi(name.substr(0, 8).c_str());
    return outNum > 0;
}


// Top-level only under Base Path
static int FindMaxProjectNumberTopLevel(const std::wstring& base)
{
    int maxNum = 0;
    std::wstring pattern = base + L"\\*";
    WIN32_FIND_DATAW ffd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE) return 0;


    do {
        if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const wchar_t* name = ffd.cFileName;
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;


        int n = 0;
        if (TryParse8DigitPrefix(name, n)) {
            if (n > maxNum) maxNum = n;
        }
    } while (FindNextFileW(h, &ffd));


    FindClose(h);
    return maxNum;
}


static bool CopyDirectoryRecursive(const std::wstring& src, const std::wstring& dst)
{
    if (!DirectoryExists(dst)) {
        if (!CreateDirectoryW(dst.c_str(), nullptr)) {
            DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS) return false;
        }
    }


    std::wstring pattern = src + L"\\*";
    WIN32_FIND_DATAW ffd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE) return false;


    bool ok = true;


    do {
        const wchar_t* name = ffd.cFileName;
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;


        std::wstring srcItem = src + L"\\" + name;
        std::wstring dstItem = dst + L"\\" + name;


        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!CopyDirectoryRecursive(srcItem, dstItem)) { ok = false; break; }
        }
        else {
            if (!CopyFileW(srcItem.c_str(), dstItem.c_str(), TRUE)) { ok = false; break; }
        }


    } while (FindNextFileW(h, &ffd));


    FindClose(h);
    return ok;
}


static bool PromptText(HWND owner, const wchar_t* title, const wchar_t* label, std::wstring& outText)
{
    struct InputState {
        HWND hEdit = nullptr;
        bool ok = false;
        std::wstring text;
        const wchar_t* label = nullptr;
    } st;
    st.label = label;


    const wchar_t* cls = L"ProjectOpenerInputDlg";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l)->LRESULT {
            InputState* s = (InputState*)GetWindowLongPtrW(h, GWLP_USERDATA);
            switch (m) {
            case WM_CREATE: {
                auto* cs = (CREATESTRUCTW*)l;
                s = (InputState*)cs->lpCreateParams;
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);


                RECT rc{}; GetClientRect(h, &rc);
                int pad = 12;


                CreateWindowW(L"STATIC", s->label, WS_CHILD | WS_VISIBLE,
                    pad, pad, rc.right - 2 * pad, 18, h, nullptr, nullptr, nullptr);


                s->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                    pad, pad + 24, rc.right - 2 * pad, 26, h, nullptr, nullptr, nullptr);


                SendMessageW(s->hEdit, WM_SETFONT, (WPARAM)gUIFont, TRUE);


                CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE,
                    rc.right - pad - 170, rc.bottom - pad - 30, 80, 26, h, (HMENU)1, nullptr, nullptr);


                CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                    rc.right - pad - 85, rc.bottom - pad - 30, 80, 26, h, (HMENU)2, nullptr, nullptr);


                SetFocus(s->hEdit);

                // Subclass edit so Enter = OK and Esc = Cancel
                SetPropW(s->hEdit, L"PF_OLDPROC", (HANDLE)SetWindowLongPtrW(s->hEdit, GWLP_WNDPROC, (LONG_PTR)+[](HWND he, UINT mm, WPARAM ww, LPARAM ll)->LRESULT {
                    if (mm == WM_KEYDOWN) {
                        if (ww == VK_RETURN) {
                            HWND parent = GetParent(he);
                            if (parent) SendMessageW(parent, WM_COMMAND, MAKEWPARAM(1, BN_CLICKED), 0);
                            return 0;
                        }
                        if (ww == VK_ESCAPE) {
                            HWND parent = GetParent(he);
                            if (parent) SendMessageW(parent, WM_COMMAND, MAKEWPARAM(2, BN_CLICKED), 0);
                            return 0;
                        }
                    }
                    WNDPROC oldp = (WNDPROC)GetPropW(he, L"PF_OLDPROC");
                    return CallWindowProcW(oldp ? oldp : DefWindowProcW, he, mm, ww, ll);
                    }));

                return 0;
            }
            case WM_COMMAND: {
                int id = LOWORD(w);
                if (id == 1) {
                    wchar_t buf[512]{};
                    GetWindowTextW(s->hEdit, buf, _countof(buf));
                    s->text = TrimSpaces(buf);
                    s->ok = true;
                    DestroyWindow(h);
                    return 0;
                }
                if (id == 2) {
                    int r = MessageBoxW(h, L"Are you sure you want to cancel the document?", L"Cancel", MB_YESNO | MB_ICONQUESTION);
                    if (r == IDYES) {
                        DestroyWindow(h);
                    }
                    return 0;
                }
                return 0;
            }
            case WM_CLOSE:
                DestroyWindow(h);
                return 0;
            }
            return DefWindowProcW(h, m, w, l);
            };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }


    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        cls, title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        520, 170,
        owner, nullptr, GetModuleHandleW(nullptr),
        &st);


    if (!dlg) return false;


    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);


    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);


    if (st.ok) { outText = st.text; return true; }
    return false;
}


static bool PromptTextMultiline(HWND owner, const wchar_t* title, const wchar_t* label, std::wstring& outText)
{
    struct InputState {
        HWND hEdit = nullptr;
        bool ok = false;
        std::wstring text;
        const wchar_t* label = nullptr;
    } st;
    st.label = label;

    const wchar_t* cls = L"ProjectOpenerInputDlg_ML";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l)->LRESULT {
            InputState* s = (InputState*)GetWindowLongPtrW(h, GWLP_USERDATA);
            switch (m) {
            case WM_CREATE: {
                auto* cs = (CREATESTRUCTW*)l;
                s = (InputState*)cs->lpCreateParams;
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);

                RECT rc{}; GetClientRect(h, &rc);
                const int pad = 12;

                CreateWindowW(L"STATIC", s->label, WS_CHILD | WS_VISIBLE,
                    pad, pad, rc.right - 2 * pad, 18, h, nullptr, nullptr, nullptr);

                // Multiline edit that accepts Enter (ES_WANTRETURN)
                s->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
                    pad, pad + 24, rc.right - 2 * pad, 24, h, nullptr, nullptr, nullptr);

                if (gUIFont) SendMessageW(s->hEdit, WM_SETFONT, (WPARAM)gUIFont, TRUE);

                CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                    rc.right - (pad + 180), rc.bottom - (pad + 30), 80, 26,
                    h, (HMENU)(INT_PTR)1, nullptr, nullptr);

                CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                    rc.right - (pad + 90), rc.bottom - (pad + 30), 80, 26,
                    h, (HMENU)(INT_PTR)2, nullptr, nullptr);

                SetFocus(s->hEdit);

                // Subclass edit so Enter = OK and Esc = Cancel
                SetPropW(s->hEdit, L"PF_OLDPROC", (HANDLE)SetWindowLongPtrW(s->hEdit, GWLP_WNDPROC, (LONG_PTR)+[](HWND he, UINT mm, WPARAM ww, LPARAM ll)->LRESULT {
                    if (mm == WM_KEYDOWN) {
                        if (ww == VK_RETURN) {
                            HWND parent = GetParent(he);
                            if (parent) SendMessageW(parent, WM_COMMAND, MAKEWPARAM(1, BN_CLICKED), 0);
                            return 0;
                        }
                        if (ww == VK_ESCAPE) {
                            HWND parent = GetParent(he);
                            if (parent) SendMessageW(parent, WM_COMMAND, MAKEWPARAM(2, BN_CLICKED), 0);
                            return 0;
                        }
                    }
                    WNDPROC oldp = (WNDPROC)GetPropW(he, L"PF_OLDPROC");
                    return CallWindowProcW(oldp ? oldp : DefWindowProcW, he, mm, ww, ll);
                    }));

                return 0;
            }
            case WM_COMMAND: {
                int id = LOWORD(w);
                if (id == 1) {
                    int len = GetWindowTextLengthW(s->hEdit);
                    if (len < 0) len = 0;
                    std::wstring buf;
                    buf.resize((size_t)len + 1);
                    GetWindowTextW(s->hEdit, buf.data(), len + 1);
                    buf.resize((size_t)len);
                    s->text = TrimSpaces(buf);
                    s->ok = true;
                    DestroyWindow(h);
                    return 0;
                }
                if (id == 2) {
                    int r = MessageBoxW(h, L"Are you sure you want to cancel the document?", L"Cancel", MB_YESNO | MB_ICONQUESTION);
                    if (r == IDYES) {
                        DestroyWindow(h);
                    }
                    return 0;
                }
                return 0;
            }
            case WM_CLOSE:
                DestroyWindow(h);
                return 0;
            }
            return DefWindowProcW(h, m, w, l);
            };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        cls, title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        560, 165,
        owner, nullptr, GetModuleHandleW(nullptr),
        &st);

    if (!dlg) return false;

    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);

    if (st.ok) { outText = st.text; return true; }
    return false;
}





static void CreateNewProjectFromTemplate()
{
    std::wstring base = GetActiveBasePath();
    if (base.empty() || !DirectoryExists(base)) {
        MessageBoxW(gMainHwnd,
            L"Base Path is not set or not accessible.\n\nOpen Settings and set a valid folder path.",
            L"New Project", MB_ICONERROR);
        return;
    }


    if (gSettings.templatePath.empty() || !DirectoryExists(gSettings.templatePath)) {
        MessageBoxW(gMainHwnd,
            L"Template Folder is not set or not accessible.\n\nOpen Settings and set the Template Folder path.",
            L"New Project", MB_ICONERROR);
        return;
    }


    // Contractor selection is mandatory in the New Project flow.
    if (gSettings.crmFolderPath.empty()) {
        MessageBoxW(gMainHwnd,
            L"CRM Folder Path is not set.\n\nOpen Settings and set a CRM folder path.",
            L"New Project", MB_ICONERROR);
        return;
    }


    CrmRow contractor{};
    if (!CrmOpenSearchDialog(gMainHwnd, true, &contractor)) {
        // user cancelled
        return;
    }


    int maxNum = FindMaxProjectNumberTopLevel(base);
    if (maxNum <= 0) {
        MessageBoxW(gMainHwnd,
            L"Could not find any TOP-LEVEL folders under Base Path starting with 8 digits.\n\n"
            L"Expected example:\n38231324 - Something",
            L"New Project", MB_ICONERROR);
        return;
    }


    int newNum = maxNum + 1;


    std::wstring desc;
    if (!PromptTextMultiline(gMainHwnd, L"New Project", L"Enter description (folder will be: ######## - Description):", desc))
        return;


    desc = SanitizeFolderName(desc);
    if (desc.empty()) {
        MessageBoxW(gMainHwnd, L"Description cannot be empty.", L"New Project", MB_ICONERROR);
        return;
    }


    wchar_t numBuf[16]{};
    swprintf_s(numBuf, L"%08d", newNum);


    std::wstring newFolderName = std::wstring(numBuf) + L" - " + desc;
    std::wstring dst = base + L"\\" + newFolderName;

    if (DirectoryExists(dst)) {
        MessageBoxW(gMainHwnd, L"Destination folder already exists.", L"New Project", MB_ICONERROR);
        return;
    }

    // Collect per-project CRM details FIRST. If user cancels, do not create anything.
    ProjectCrmRecord rec{};
    ProjectCrmRecord prefill{};
    prefill.ProjectNo = std::wstring(numBuf);
    prefill.OwnerName = desc; // requested: prefill Owner Name from description (editable)
    prefill.CreatedOn = NowCreatedOn();

    if (!ProjectCrmOpenDialog(gMainHwnd, std::wstring(numBuf), contractor, rec, &prefill)) {
        // user cancelled (confirmation is handled inside the CRM dialog)
        return;
    }

    // Create project folder and copy template only after CRM is confirmed.
    if (!CreateDirectoryW(dst.c_str(), nullptr)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) {
            MessageBoxW(gMainHwnd, L"Failed to create destination folder.", L"New Project", MB_ICONERROR);
            return;
        }
    }

    if (!CopyDirectoryRecursive(gSettings.templatePath, dst)) {
        MessageBoxW(gMainHwnd,
            L"Copy failed (Template Folder -> New Project).\n\n"
            L"Check permissions and that files are not locked.",
            L"New Project", MB_ICONERROR);

        // Roll back folder if copy failed
        DeleteDirectoryRecursive(dst);
        return;
    }

    if (!SaveProjectCrmOutputs(dst, rec)) {
        MessageBoxW(gMainHwnd,
            L"Project created, but CRM .csv output failed.\n\nCheck CRM Folder Path and permissions.",
            L"CRM", MB_OK | MB_ICONWARNING);
    }

    OpenFolderInExplorer(dst);
    AddRecentFolder(dst);
}

// -------------------- Settings window --------------------

struct SettingsState {
    HWND parent = nullptr;
    HWND editBase = nullptr;
    HWND editTemplate = nullptr;
    HWND editCrm = nullptr;
    HWND editContractor = nullptr;
    HWND bgSample = nullptr;
    HWND fgSample = nullptr;
    HWND chkSearch = nullptr;

    // Donate button
    HWND btnDonate = nullptr;
    HBITMAP hbmpDonate = nullptr;
    int donateW = 220;
    int donateH = 70;

    bool done = false;
    UiSettings working;
};


static void SetSampleText(HWND h, const wchar_t* label, COLORREF c)
{
    wchar_t buf[128]{};
    swprintf_s(buf, L"%s  (R:%d G:%d B:%d)", label, GetRValue(c), GetGValue(c), GetBValue(c));
    SetWindowTextW(h, buf);
}


static bool IsChecked(HWND hDlg, int id)
{
    HWND h = GetDlgItem(hDlg, id);
    return (SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED);
}


static void SetChecked(HWND hDlg, int id, bool v)
{
    HWND h = GetDlgItem(hDlg, id);
    SendMessageW(h, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
}


static HWND CreateIconCheckbox(HWND parent, int id, const wchar_t* glyph, int x, int y)
{
    HWND hCtl = CreateWindowW(L"BUTTON", glyph,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        BS_AUTOCHECKBOX | BS_PUSHLIKE | BS_FLAT | BS_CENTER | BS_VCENTER,
        x, y, 48, 28,
        parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);

    ApplyFont(hCtl, gIconFont);
    return hCtl;
}



static HWND CreateSettingsTileButton(HWND parent, int id, const wchar_t* label, int x, int y, int w, int h)
{
    HWND b = CreateWindowW(L"BUTTON", label,
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        x, y, w, h,
        parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    ApplyFont(b, gUIFont);
    return b;
}




// -------------------- About window --------------------


static LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        RECT rc{}; GetClientRect(hwnd, &rc);

        const int pad = 12;
        const int btnH = 28;
        const int btnW1 = 160;
        const int btnW2 = 140;

        int yBtn = rc.bottom - pad - btnH;

        // Buttons at bottom
        HWND hBtnUpdate = CreateWindowW(L"BUTTON", L"Check for updates",
            WS_CHILD | WS_VISIBLE,
            pad, yBtn, btnW1, btnH,
            hwnd, (HMENU)(INT_PTR)ID_ABOUT_UPDATE, GetModuleHandleW(nullptr), nullptr);

        HWND hBtnOld = CreateWindowW(L"BUTTON", L"Old versions",
            WS_CHILD | WS_VISIBLE,
            pad + btnW1 + 10, yBtn, btnW2, btnH,
            hwnd, (HMENU)(INT_PTR)ID_ABOUT_OLD, GetModuleHandleW(nullptr), nullptr);

        // Link line just above the buttons
        const int linkH = 20;
        int yLink = yBtn - 8 - linkH;

        // About text area (read-only) - STOP above the link
        int editH = yLink - pad - 6; // leaves a little gap before the link
        HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            ABOUT_TEXT,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            pad, pad, rc.right - 2 * pad, editH,
            hwnd, (HMENU)1, GetModuleHandleW(nullptr), nullptr);

        // Clickable email link (create AFTER edit so it can't be covered)
        HWND hLink = CreateWindowW(
            L"SysLink",
            L"Support / Contact: <a href=\"mailto:info.pah@mail.com\">info.pah@mail.com</a>",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            pad, yLink, rc.right - 2 * pad, linkH,
            hwnd, (HMENU)(INT_PTR)ID_ABOUT_EMAIL, GetModuleHandleW(nullptr), nullptr

        );

        // Fonts
        ApplyFont(hEdit, gUIFont);
        if (hLink) ApplyFont(hLink, gUIFont);
        if (hBtnUpdate) ApplyFont(hBtnUpdate, gUIFont);
        if (hBtnOld) ApplyFont(hBtnOld, gUIFont);

        return 0;
    }

    case WM_NOTIFY:
    {
        LPNMHDR hdr = (LPNMHDR)lParam;
        if (hdr && hdr->idFrom == ID_ABOUT_EMAIL &&
            (hdr->code == NM_CLICK || hdr->code == NM_RETURN))
        {
            ShellExecuteW(hwnd, L"open", L"mailto:info.pah@mail.com", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    }


    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == ID_ABOUT_UPDATE) {
            CheckForUpdates_Ask(hwnd, false);
            return 0;
        }
        if (id == ID_ABOUT_OLD) {
            ShellExecuteW(nullptr, L"open", kUpdateReleasesPageUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        return 0;
    }
    case WM_SIZE:
    {
        RECT rc{}; GetClientRect(hwnd, &rc);
        const int pad = 12;
        const int btnH = 28;
        const int btnW1 = 160;
        const int btnW2 = 140;
        int yBtn = rc.bottom - pad - btnH;

        HWND b1 = GetDlgItem(hwnd, ID_ABOUT_UPDATE);
        HWND b2 = GetDlgItem(hwnd, ID_ABOUT_OLD);
        if (b1) SetWindowPos(b1, nullptr, pad, yBtn, btnW1, btnH, SWP_NOZORDER);
        if (b2) SetWindowPos(b2, nullptr, pad + btnW1 + 10, yBtn, btnW2, btnH, SWP_NOZORDER);

        HWND hLink = GetDlgItem(hwnd, ID_ABOUT_EMAIL);
        const int linkH = 20;
        int yLink = yBtn - 8 - linkH;
        if (hLink) SetWindowPos(hLink, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

        HWND edit = GetDlgItem(hwnd, 1);
        if (edit) {
            int editH = yLink - pad - pad;
            SetWindowPos(edit, nullptr, pad, pad, rc.right - 2 * pad, editH, SWP_NOZORDER);
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}


static void ShowAboutWindow(HWND owner)
{
    static bool sReg = false;
    if (!sReg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = AboutProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ProjectFinderAboutWnd";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        sReg = true;
    }


    // Match Settings window size roughly
    const int w = 620;
    const int h = 460;


    RECT rcOwner{}; GetWindowRect(owner ? owner : gMainHwnd, &rcOwner);
    int x = rcOwner.left + 40;
    int y = rcOwner.top + 40;


    HWND wAbout = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ProjectFinderAboutWnd", L"About",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, w, h, owner ? owner : gMainHwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    (void)wAbout;
}


static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Grab state (may be null until WM_NCCREATE runs)
    auto* st = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // Set state as early as possible
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        st = reinterpret_cast<SettingsState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        return TRUE;
    }

    // If we still don't have state, do default handling (prevents crashes/auto-close)
    if (!st)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg)
    {
    case WM_CTLCOLORBTN:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, gSettings.fg);   // usually white in your theme
        SetBkMode(hdc, TRANSPARENT);
        return (INT_PTR)gBgBrush;
    }

    case WM_CREATE:
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);



        const int pad = 12;


        // Base Path
        CreateWindowW(L"STATIC",
            L"Base Path (required):",
            WS_CHILD | WS_VISIBLE,
            pad, 12, 520, 18, hwnd, nullptr, nullptr, nullptr);


        st->editBase = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            st->working.baseOverride.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            pad, 34, 470, 26, hwnd, (HMENU)(INT_PTR)ID_SET_EDIT_BASE, nullptr, nullptr);
        ApplyFont(st->editBase, gUIFont);


        CreateSettingsTileButton(hwnd, ID_SET_BROWSE_BASE, L"Browse...", pad + 480, 34, 110, 26);


        // Template Path
        CreateWindowW(L"STATIC",
            L"Template Folder (copied for New Project):",
            WS_CHILD | WS_VISIBLE,
            pad, 66, 520, 18, hwnd, nullptr, nullptr, nullptr);


        st->editTemplate = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            st->working.templatePath.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            pad, 88, 470, 26, hwnd, (HMENU)(INT_PTR)ID_SET_EDIT_TEMPLATE, nullptr, nullptr);
        ApplyFont(st->editTemplate, gUIFont);


        CreateSettingsTileButton(hwnd, ID_SET_BROWSE_TEMPLATE, L"Browse...", pad + 480, 88, 110, 26);


        // CRM Folder Path
        CreateWindowW(L"STATIC",
            L"Project List CRM (stores Project CRM.csv):",
            WS_CHILD | WS_VISIBLE,
            pad, 120, 520, 18, hwnd, nullptr, nullptr, nullptr);


        st->editCrm = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            st->working.crmFolderPath.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            pad, 142, 470, 26, hwnd, (HMENU)(INT_PTR)ID_SET_EDIT_CRM, nullptr, nullptr);
        ApplyFont(st->editCrm, gUIFont);


        CreateSettingsTileButton(hwnd, ID_SET_BROWSE_CRM, L"Browse...", pad + 480, 142, 110, 26);


        // Contractor Contact List.csv
        CreateWindowW(L"STATIC",
            L"Contractor Contact List.csv (contractor-only):",
            WS_CHILD | WS_VISIBLE,
            pad, 174, 520, 18, hwnd, nullptr, nullptr, nullptr);


        st->editContractor = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            st->working.contractorCsvPath.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            pad, 196, 470, 26, hwnd, (HMENU)(INT_PTR)ID_SET_EDIT_CONTRACTOR, nullptr, nullptr);
        ApplyFont(st->editContractor, gUIFont);


        CreateSettingsTileButton(hwnd, ID_SET_BROWSE_CONTRACTOR, L"Browse...", pad + 480, 196, 110, 26);


        // Icon checkboxes row
        CreateWindowW(L"STATIC", L"Show icons (tick = visible):",
            WS_CHILD | WS_VISIBLE,
            pad, 230, 260, 18, hwnd, nullptr, nullptr, nullptr);


        int x = pad;
        int y = 252;
        CreateIconCheckbox(hwnd, ID_CHK_FLAG, L"\uE7C1", x, y); x += 58; // Recent
        CreateIconCheckbox(hwnd, ID_CHK_FOLDER, L"\uE8B7", x, y); x += 58; // Folder
        CreateIconCheckbox(hwnd, ID_CHK_DOC, L"\uE8A5", x, y); x += 58; // DOC
        // CRM icon removed: CRM is now accessed via the Phone button (per-company CSV).
        CreateIconCheckbox(hwnd, ID_CHK_MAIL, L"\uE715", x, y); x += 58; // Mail
        CreateIconCheckbox(hwnd, ID_CHK_CAL, L"\uE787", x, y); x += 58; // Calendar
        CreateIconCheckbox(hwnd, ID_CHK_PHONE, L"\uE717", x, y); x += 58; // Phone
        CreateIconCheckbox(hwnd, ID_CHK_GEAR, L"\uE713", x, y); x += 58; // Settings

        ApplyFont(GetDlgItem(hwnd, ID_CHK_FLAG), gIconFont);
        ApplyFont(GetDlgItem(hwnd, ID_CHK_FOLDER), gIconFont);
        ApplyFont(GetDlgItem(hwnd, ID_CHK_DOC), gIconFont);
        ApplyFont(GetDlgItem(hwnd, ID_CHK_MAIL), gIconFont);
        ApplyFont(GetDlgItem(hwnd, ID_CHK_CAL), gIconFont);
        ApplyFont(GetDlgItem(hwnd, ID_CHK_PHONE), gIconFont);
        ApplyFont(GetDlgItem(hwnd, ID_CHK_GEAR), gIconFont);

        SetChecked(hwnd, ID_CHK_FLAG, st->working.showFlag);
        SetChecked(hwnd, ID_CHK_FOLDER, st->working.showFolder);
        SetChecked(hwnd, ID_CHK_DOC, st->working.showDoc);
        SetChecked(hwnd, ID_CHK_MAIL, st->working.showMail);
        SetChecked(hwnd, ID_CHK_CAL, st->working.showCal);
        SetChecked(hwnd, ID_CHK_PHONE, st->working.showPhone);
        SetChecked(hwnd, ID_CHK_GEAR, st->working.showGear);
        // Search-anywhere removed per spec (search folder name only)




        CreateSettingsTileButton(hwnd, ID_SET_ABOUT, L"About", 330, 300, 80, 28);
        CreateSettingsTileButton(hwnd, ID_SET_OK, L"OK", 420, 300, 80, 28);
        CreateSettingsTileButton(hwnd, ID_SET_CANCEL, L"Cancel", 510, 300, 80, 28);

        // Donate button (bottom-left)
        st->btnDonate = CreateWindowW(
            L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_BITMAP,
            0, 0, 10, 10,
            hwnd, (HMENU)(INT_PTR)ID_SET_DONATE, GetModuleHandleW(nullptr), nullptr
        );


        int wPng = 0, hPng = 0;
        HBITMAP original = LoadPngResourceAsHBITMAP(IDB_PNG1, &wPng, &hPng);

        st->donateW = 140;
        st->donateH = 44;

        if (original) {
            HBITMAP scaled = ResizeHBITMAP(original, st->donateW, st->donateH);
            DeleteObject(original); // ok to delete original

            st->hbmpDonate = scaled ? scaled : nullptr;

            if (st->hbmpDonate) {
                SendMessageW(st->btnDonate, BM_SETIMAGE, IMAGE_BITMAP, (LPARAM)st->hbmpDonate);
                InvalidateRect(st->btnDonate, nullptr, TRUE);
                UpdateWindow(st->btnDonate);
            }
            else {
                SetWindowTextW(st->btnDonate, L"Donate");
                ApplyFont(st->btnDonate, gUIFont);
            }
        }
        else {
            SetWindowTextW(st->btnDonate, L"Donate");
            ApplyFont(st->btnDonate, gUIFont);
        }


        LayoutDonateButton(hwnd, st->btnDonate, st->donateW, st->donateH);

        PostMessageW(hwnd, WM_NEXTDLGCTL, (WPARAM)st->editBase, TRUE);
        return 0;
    }


    case WM_SIZE:
        LayoutDonateButton(hwnd, st ? st->btnDonate : nullptr, st ? st->donateW : 0, st ? st->donateH : 0);
        return 0;


    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT r; GetClientRect(hwnd, &r);
        FillRect(hdc, &r, gBgBrush);
        return TRUE;
    }


    case WM_CTLCOLORDLG:
        return (INT_PTR)gBgBrush;


    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, gSettings.fg);
        SetBkMode(hdc, TRANSPARENT);
        return (INT_PTR)gBgBrush;
    }


    // Make Settings edits white too
    case WM_CTLCOLOREDIT:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, OPAQUE);
        return (INT_PTR)(gEditWhiteBrush ? gEditWhiteBrush : (HBRUSH)GetStockObject(WHITE_BRUSH));
    }


    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
        if (!dis || dis->CtlType != ODT_BUTTON) break;

        const int cid = (int)dis->CtlID;

        // Icon toggle buttons (checkbox row)
        if (cid == ID_CHK_FLAG || cid == ID_CHK_FOLDER || cid == ID_CHK_DOC ||
            cid == ID_CHK_MAIL || cid == ID_CHK_CAL || cid == ID_CHK_PHONE || cid == ID_CHK_GEAR)
        {
            wchar_t glyph[16]{};
            GetWindowTextW(dis->hwndItem, glyph, _countof(glyph));

            bool checked = (SendMessageW(dis->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED);
            bool disabled = (dis->itemState & ODS_DISABLED) != 0;
            bool focused = (dis->itemState & ODS_FOCUS) != 0;

            // Use your existing tile drawer, but with the icon font.
            // Treat "checked" like "pressed" so it looks toggled on.
            DrawFancyIconTile(dis->hDC, dis->rcItem, glyph, gIconFont, false,
                checked, disabled, focused);
            return TRUE;
        }

        // --- keep your existing code below for normal buttons ---


        // Default: draw regular settings buttons (Browse/About/OK/Cancel) as tiles
        wchar_t cap[128]{};
        GetWindowTextW(dis->hwndItem, cap, _countof(cap));

        bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        bool disabled = (dis->itemState & ODS_DISABLED) != 0;
        bool focused = (dis->itemState & ODS_FOCUS) != 0;

        DrawFancyIconTile(dis->hDC, dis->rcItem, cap, gUIFont, false, pressed, disabled, focused);
        return TRUE;
    }


    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);

        // IMPORTANT: ignore non-click notifications (focus, edit change, etc.)
        if (code != BN_CLICKED)
            return 0;

        if (id == ID_SET_DONATE) {
            ShellExecuteW(hwnd, L"open", kDonateUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }



        if (id == ID_SET_BROWSE_BASE) {
            std::wstring p;
            wchar_t buf[4096]{};
            GetWindowTextW(st->editBase, buf, _countof(buf));
            p = TrimSpaces(buf);


            if (BrowseForFolder(hwnd, p, L"Select Base Path")) {
                SetWindowTextW(st->editBase, p.c_str());
                SetFocus(st->editBase);
            }
            return 0;
        }


        if (id == ID_SET_BROWSE_TEMPLATE) {
            std::wstring p;
            wchar_t buf[4096]{};
            GetWindowTextW(st->editTemplate, buf, _countof(buf));
            p = TrimSpaces(buf);


            if (BrowseForFolder(hwnd, p, L"Select Template Folder")) {
                SetWindowTextW(st->editTemplate, p.c_str());
                SetFocus(st->editTemplate);
            }
            return 0;
        }


        if (id == ID_SET_BROWSE_CRM) {
            std::wstring p;
            wchar_t buf[4096]{};
            GetWindowTextW(st->editCrm, buf, _countof(buf));
            p = TrimSpaces(buf);


            if (BrowseForFolder(hwnd, p, L"Select CRM Folder")) {
                SetWindowTextW(st->editCrm, p.c_str());
                SetFocus(st->editCrm);
            }
            return 0;
        }




        if (id == ID_SET_BROWSE_CONTRACTOR) {
            std::wstring p;
            wchar_t buf2[4096]{};
            GetWindowTextW(st->editContractor, buf2, _countof(buf2));
            p = TrimSpaces(buf2);


            // Select folder for contractor list; file will be created inside it.
            if (BrowseForFolder(hwnd, p, L"Select folder for Contractor Contact List.csv")) {
                if (!p.empty()) {
                    if (p.back() == L'\\' || p.back() == L'/') p.pop_back();
                    p += L"\\Contractor Contact List.csv";
                }
                SetWindowTextW(st->editContractor, p.c_str());
                SetFocus(st->editContractor);
            }
            return 0;
        }


        if (id == ID_SET_BG_BTN) {
            if (PickColor(hwnd, st->working.bg)) SetSampleText(st->bgSample, L"BG", st->working.bg);
            return 0;
        }


        if (id == ID_SET_FG_BTN) {
            if (PickColor(hwnd, st->working.fg)) SetSampleText(st->fgSample, L"Text", st->working.fg);
            return 0;
        }


        if (id == ID_SET_ABOUT) { ShowAboutWindow(hwnd); return 0; }


        if (id == ID_SET_OK) {
            wchar_t baseBuf[4096]{};
            GetWindowTextW(st->editBase, baseBuf, _countof(baseBuf));
            st->working.baseOverride = TrimSpaces(baseBuf);


            wchar_t tplBuf[4096]{};
            GetWindowTextW(st->editTemplate, tplBuf, _countof(tplBuf));
            st->working.templatePath = TrimSpaces(tplBuf);


            wchar_t crmBuf[4096]{};
            GetWindowTextW(st->editCrm, crmBuf, _countof(crmBuf));
            st->working.crmFolderPath = TrimSpaces(crmBuf);


            wchar_t conBuf[4096]{};
            GetWindowTextW(st->editContractor, conBuf, _countof(conBuf));
            st->working.contractorCsvPath = TrimSpaces(conBuf);


            // CRM folder: create if it doesn't exist (but must be a folder path).
            if (!st->working.crmFolderPath.empty()) {
                if (!DirectoryExists(st->working.crmFolderPath)) {
                    CreateDirectoryW(st->working.crmFolderPath.c_str(), nullptr);
                }
            }


            st->working.showFlag = IsChecked(hwnd, ID_CHK_FLAG);
            st->working.showFolder = IsChecked(hwnd, ID_CHK_FOLDER);
            st->working.showDoc = IsChecked(hwnd, ID_CHK_DOC);
            st->working.showCRM = false; // CRM icon removed; use Phone button.
            st->working.showMail = IsChecked(hwnd, ID_CHK_MAIL);
            st->working.showCal = IsChecked(hwnd, ID_CHK_CAL);
            st->working.showPhone = IsChecked(hwnd, ID_CHK_PHONE);
            st->working.showGear = IsChecked(hwnd, ID_CHK_GEAR);


            st->working.searchAnywhere = false; // removed per spec


            if (!st->working.showGear) {
                MessageBoxW(hwnd, L"Gear icon cannot be hidden (Settings must remain accessible).", L"Notice", MB_OK);
                st->working.showGear = true;
                SetChecked(hwnd, ID_CHK_GEAR, true);
                return 0;
            }


            if (!st->working.showPhone) {
                MessageBoxW(hwnd, L"Phone icon cannot be hidden (CRM uses Phone).", L"Notice", MB_OK);
                st->working.showPhone = true;
                SetChecked(hwnd, ID_CHK_PHONE, true);
                return 0;
            }


            gSettings = st->working;
            SaveUiSettings();
            RebuildBrush();
            LayoutMainBar();
            ApplySearchModeToEdit();


            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }


        if (id == ID_SET_CANCEL) {
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }


        return 0;
    }


    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;


    case WM_DESTROY:
        if (st) {
            if (st->hbmpDonate) {
                DeleteObject(st->hbmpDonate);
                st->hbmpDonate = nullptr;
            }
            if (st->parent) EnableWindow(st->parent, TRUE);
        }
        return 0;
    }


    return DefWindowProcW(hwnd, msg, wParam, lParam);
}


static void OpenSettingsDialog(HWND parent)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kSettingsClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }


    SettingsState st{};
    st.parent = parent;
    st.working = gSettings;


    EnableWindow(parent, FALSE);


    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kSettingsClass,
        L"Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        620, 390,
        parent, nullptr, GetModuleHandleW(nullptr),
        &st);


    if (!dlg) {
        EnableWindow(parent, TRUE);
        MessageBoxW(parent, L"Failed to open Settings window.", L"Error", MB_ICONERROR);
        return;
    }


    RECT dr{};
    GetWindowRect(dlg, &dr);


    int dlgW = dr.right - dr.left;
    int dlgH = dr.bottom - dr.top;


    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);


    int px = (screenW - dlgW) / 2;
    int py = (screenH - dlgH) / 2;


    SetWindowPos(dlg, nullptr, px, py, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);


    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);


    MSG m;
    while (IsWindow(dlg) && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}


// ============================================================================
// DOC WINDOW (RESTORED)
// ============================================================================


// DOC control IDs
static const int ID_DOC_LISTVIEW = 8001;
static const int ID_DOC_ADDLINE = 8002;
static const int ID_DOC_SAVE = 8003;
static const int ID_DOC_FILTER_STATUS = 8004;
static const int ID_DOC_FILTER_TEXT = 8005;
static const int ID_DOC_OVERLAY_EDIT = 8100;


// ListView columns
enum DocCols
{
    DOC_COL_OPEN = 0,
    DOC_COL_JOB = 1,
    DOC_COL_DESC = 2,
    DOC_COL_SCOPE = 3,
    DOC_COL_DUE = 4,
    DOC_COL_STAT = 5,
    DOC_COL_DEL = 6,
};


// Data row
struct ProjectRow
{
    std::wstring job;   // can be 8 digits OR B00#### OR 4 digits
    std::wstring desc;
    std::wstring scope;
    bool hasDue = false;
    SYSTEMTIME due{};
    int status = 1;
};


struct DocState
{
    HWND hwnd = nullptr;
    HWND lv = nullptr;


    HWND btnAdd = nullptr;
    HWND btnSave = nullptr;
    HWND editStatus = nullptr;
    HWND editText = nullptr;


    HWND overlay = nullptr;
    int editViewRow = -1;
    int editCol = -1;


    HIMAGELIST rowHeightIL = nullptr;


    std::vector<ProjectRow> rows; // model
};


static DocState gDoc;
static WNDPROC gOldDocOverlayProc = nullptr;


static std::wstring GetAppDataPath()
{
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    return std::wstring(buf);
}


static std::wstring GetSettingsIniPath()
{
    std::wstring dir = GetAppDataPath() + L"\\ProjectOpener";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}


static void LoadUiSettings()
{
    std::wstring ini = GetSettingsIniPath();


    wchar_t buf[4096]{};


    GetPrivateProfileStringW(L"Paths", L"Base", L"", buf, _countof(buf), ini.c_str());
    gSettings.baseOverride = TrimSpaces(buf);


    GetPrivateProfileStringW(L"Paths", L"Template", L"", buf, _countof(buf), ini.c_str());
    gSettings.templatePath = TrimSpaces(buf);


    GetPrivateProfileStringW(L"Paths", L"CRMFolder", L"", buf, _countof(buf), ini.c_str());
    gSettings.crmFolderPath = TrimSpaces(buf);


    GetPrivateProfileStringW(L"Paths", L"ContractorCsv", L"", buf, _countof(buf), ini.c_str());
    gSettings.contractorCsvPath = TrimSpaces(buf);


    gSettings.showFlag = GetPrivateProfileIntW(L"Icons", L"Flag", gSettings.showFlag ? 1 : 0, ini.c_str()) != 0;
    gSettings.showFolder = GetPrivateProfileIntW(L"Icons", L"Folder", gSettings.showFolder ? 1 : 0, ini.c_str()) != 0;
    gSettings.showDoc = GetPrivateProfileIntW(L"Icons", L"Doc", gSettings.showDoc ? 1 : 0, ini.c_str()) != 0;
    gSettings.showCRM = false; // CRM icon removed; use Phone button.
    gSettings.showMail = GetPrivateProfileIntW(L"Icons", L"Mail", gSettings.showMail ? 1 : 0, ini.c_str()) != 0;
    gSettings.showCal = GetPrivateProfileIntW(L"Icons", L"Cal", gSettings.showCal ? 1 : 0, ini.c_str()) != 0;
    gSettings.showPhone = true; // Phone is the CRM entry point.
    gSettings.showGear = GetPrivateProfileIntW(L"Icons", L"Gear", gSettings.showGear ? 1 : 0, ini.c_str()) != 0;


    gSettings.searchAnywhere = false; // removed


    gSettings.bg = (COLORREF)(DWORD)GetPrivateProfileIntW(L"Theme", L"BG", (int)gSettings.bg, ini.c_str());
    gSettings.fg = (COLORREF)(DWORD)GetPrivateProfileIntW(L"Theme", L"FG", (int)gSettings.fg, ini.c_str());


    // Safety: settings icon must remain accessible
    if (!gSettings.showGear) gSettings.showGear = true;


    // Options
    GetPrivateProfileStringW(L"Options", L"UseDescAsDefaultName", L"0", buf, _countof(buf), ini.c_str());
    gSettings.useDescAsDefaultName = (wcstol(buf, nullptr, 10) != 0);

    GetPrivateProfileStringW(L"Options", L"EnableSmartStatus", L"0", buf, _countof(buf), ini.c_str());
    gSettings.enableSmartStatus = (wcstol(buf, nullptr, 10) != 0);

    GetPrivateProfileStringW(L"Options", L"EnableAdvancedSearch", L"0", buf, _countof(buf), ini.c_str());
    gSettings.enableAdvancedSearch = (wcstol(buf, nullptr, 10) != 0);

    GetPrivateProfileStringW(L"Options", L"BackupEveryDays", L"0", buf, _countof(buf), ini.c_str());
    gSettings.backupEveryDays = (int)wcstol(buf, nullptr, 10);

    GetPrivateProfileStringW(L"Options", L"SoftDeleteEnabled", L"1", buf, _countof(buf), ini.c_str());
    gSettings.softDeleteEnabled = (wcstol(buf, nullptr, 10) != 0);
}


static void SaveUiSettings()
{
    std::wstring ini = GetSettingsIniPath();


    WritePrivateProfileStringW(L"Paths", L"Base",
        gSettings.baseOverride.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Paths", L"Template",
        gSettings.templatePath.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Paths", L"CRMFolder",
        gSettings.crmFolderPath.c_str(), ini.c_str());


    WritePrivateProfileStringW(L"Paths", L"ContractorCsv",
        gSettings.contractorCsvPath.c_str(), ini.c_str());



    // Options
    WritePrivateProfileStringW(L"Options", L"UseDescAsDefaultName",
        gSettings.useDescAsDefaultName ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Options", L"EnableSmartStatus",
        gSettings.enableSmartStatus ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Options", L"EnableAdvancedSearch",
        gSettings.enableAdvancedSearch ? L"1" : L"0", ini.c_str());
    {
        wchar_t b[32]{};
        swprintf_s(b, L"%d", gSettings.backupEveryDays);
        WritePrivateProfileStringW(L"Options", L"BackupEveryDays", b, ini.c_str());
    }
    WritePrivateProfileStringW(L"Options", L"SoftDeleteEnabled",
        gSettings.softDeleteEnabled ? L"1" : L"0", ini.c_str());

    auto wpi = [&](const wchar_t* sec, const wchar_t* key, int v)
        {
            wchar_t b[32]{};
            swprintf_s(b, L"%d", v);
            WritePrivateProfileStringW(sec, key, b, ini.c_str());
        };


    wpi(L"Icons", L"Flag", gSettings.showFlag ? 1 : 0);
    wpi(L"Icons", L"Folder", gSettings.showFolder ? 1 : 0);
    wpi(L"Icons", L"Doc", gSettings.showDoc ? 1 : 0);
    // CRM icon removed; do not persist
    wpi(L"Icons", L"Mail", gSettings.showMail ? 1 : 0);
    wpi(L"Icons", L"Cal", gSettings.showCal ? 1 : 0);
    wpi(L"Icons", L"Phone", 1);
    wpi(L"Icons", L"Gear", gSettings.showGear ? 1 : 0);
    wpi(L"Theme", L"BG", (int)gSettings.bg);
    wpi(L"Theme", L"FG", (int)gSettings.fg);
}




static std::wstring GetDocListFilePath()
{
    std::wstring dir = GetAppDataPath() + L"\\ProjectOpener";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\project_list.tsv";
}


static std::wstring DigitsOnly(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) if (c >= L'0' && c <= L'9') out.push_back(c);
    return out;
}


static bool IsValidDateYMD(int y, int m, int d)
{
    if (y < 1900 || y > 3000) return false;
    if (m < 1 || m > 12) return false;
    if (d < 1 || d > 31) return false;


    SYSTEMTIME st{};
    st.wYear = (WORD)y;
    st.wMonth = (WORD)m;
    st.wDay = (WORD)d;
    FILETIME ft{};
    return SystemTimeToFileTime(&st, &ft) != 0;
}


static std::wstring FormatDue_DDMMYYYY_Dots(const SYSTEMTIME& st)
{
    wchar_t buf[32]{};
    swprintf_s(buf, L"%02d.%02d.%04d", (int)st.wDay, (int)st.wMonth, (int)st.wYear);
    return buf;
}


// Accept ddmmyyyy (8 digits) OR dd.mm.yyyy / dd-mm-yyyy / dd/mm/yyyy
static bool ParseDueFlexible(const std::wstring& raw, SYSTEMTIME& outSt)
{
    std::wstring s = TrimSpaces(raw);
    if (s.empty()) return false;


    std::wstring dig = DigitsOnly(s);
    if (dig.size() != 8) return false;


    int dd = (dig[0] - L'0') * 10 + (dig[1] - L'0');
    int mm = (dig[2] - L'0') * 10 + (dig[3] - L'0');
    int yy = (dig[4] - L'0') * 1000 + (dig[5] - L'0') * 100 + (dig[6] - L'0') * 10 + (dig[7] - L'0');


    if (!IsValidDateYMD(yy, mm, dd)) return false;


    SYSTEMTIME st{};
    st.wYear = (WORD)yy;
    st.wMonth = (WORD)mm;
    st.wDay = (WORD)dd;
    outSt = st;
    return true;
}


static bool ParseDateYMD(const std::wstring& s, SYSTEMTIME& st)
{
    int y = 0, m = 0, d = 0;
    if (swscanf_s(s.c_str(), L"%d-%d-%d", &y, &m, &d) == 3) {
        if (IsValidDateYMD(y, m, d)) {
            SYSTEMTIME t{};
            t.wYear = (WORD)y;
            t.wMonth = (WORD)m;
            t.wDay = (WORD)d;
            st = t;
            return true;
        }
    }
    return false;
}


static std::wstring TsvEscape(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\t') out += L"    ";
        else if (c == L'\r' || c == L'\n') out += L' ';
        else out += c;
    }
    return out;
}


static void DocSaveToDisk()
{
    const std::wstring path = GetDocListFilePath();
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (!f) return;


    const wchar_t bom = 0xFEFF;
    fwrite(&bom, sizeof(wchar_t), 1, f);


    for (const auto& r : gDoc.rows) {
        wchar_t dueBuf[32]{};
        if (r.hasDue) {
            swprintf_s(dueBuf, L"%04d-%02d-%02d", r.due.wYear, r.due.wMonth, r.due.wDay);
        }


        std::wstring line =
            TsvEscape(r.job) + L"\t" +
            TsvEscape(r.desc) + L"\t" +
            TsvEscape(r.scope) + L"\t" +
            dueBuf + L"\t" +
            std::to_wstring(r.status) + L"\r\n";


        fwrite(line.c_str(), sizeof(wchar_t), line.size(), f);
    }


    fclose(f);
}


static void DocLoadFromDisk()
{
    gDoc.rows.clear();


    const std::wstring path = GetDocListFilePath();
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return;


    wchar_t first{};
    size_t r1 = fread(&first, sizeof(wchar_t), 1, f);
    if (r1 == 1) {
        if (first != 0xFEFF) fseek(f, 0, SEEK_SET);
    }


    wchar_t buf[8192]{};
    std::wstring all;
    while (true) {
        size_t n = fread(buf, sizeof(wchar_t), _countof(buf) - 1, f);
        if (n == 0) break;
        buf[n] = 0;
        all.append(buf, buf + n);
    }
    fclose(f);


    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(L"\n", pos);
        if (end == std::wstring::npos) end = all.size();
        std::wstring rowLine = all.substr(pos, end - pos);
        if (!rowLine.empty() && rowLine.back() == L'\r') rowLine.pop_back();
        pos = end + 1;


        if (rowLine.empty()) continue;


        std::vector<std::wstring> cols;
        size_t cpos = 0;
        while (true) {
            size_t tab = rowLine.find(L'\t', cpos);
            if (tab == std::wstring::npos) { cols.push_back(rowLine.substr(cpos)); break; }
            cols.push_back(rowLine.substr(cpos, tab - cpos));
            cpos = tab + 1;
        }
        while (cols.size() < 5) cols.push_back(L"");


        ProjectRow r{};
        r.job = cols[0];
        r.desc = cols[1];
        r.scope = cols[2];


        SYSTEMTIME st{};
        if (ParseDateYMD(cols[3], st)) { r.hasDue = true; r.due = st; }


        r.status = _wtoi(cols[4].c_str());
        if (r.status <= 0) r.status = 5;


        gDoc.rows.push_back(r);
    }
}


static bool TryParse8Digits(const std::wstring& raw, std::wstring& out8)
{
    std::wstring s = TrimSpaces(raw);
    if (s.size() < 8) return false;
    for (int i = 0; i < 8; ++i) if (!iswdigit(s[i])) return false;
    out8 = s.substr(0, 8);
    return true;
}


static bool ResolveTopLevelBy8DigitPrefix(const std::wstring& base, const std::wstring& num8, std::wstring& outPath)
{
    if (base.empty() || !DirectoryExists(base)) return false;


    std::wstring pattern = base + L"\\" + num8 + L"*";
    WIN32_FIND_DATAW ffd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE) return false;


    bool found = false;
    do {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            const wchar_t* name = ffd.cFileName;
            if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;
            outPath = base + L"\\" + name;
            found = true;
            break;
        }
    } while (FindNextFileW(h, &ffd));


    FindClose(h);
    return found;
}


static bool DocResolveRowToPath(const ProjectRow& r, std::wstring& outPath)
{
    std::wstring base = GetActiveBasePath();
    if (base.empty() || !DirectoryExists(base)) {
        MessageBoxW(gDoc.hwnd,
            L"Base Path is not set or not accessible.\n\nOpen Settings and set a valid folder path.",
            L"DOC", MB_ICONERROR);
        return false;
    }


    // 8-digit top-level projects: "######## - Description"
    std::wstring num8;
    if (TryParse8Digits(r.job, num8)) {
        if (ResolveTopLevelBy8DigitPrefix(base, num8, outPath) && DirectoryExists(outPath))
            return true;


        std::wstring msg = L"8-digit project folder not found under Base Path:\n\n" + num8;
        MessageBoxW(gDoc.hwnd, msg.c_str(), L"DOC", MB_ICONERROR);
        return false;
    }


    // B00#### / 4 digits job format
    std::wstring job7;
    if (!ParseJob7FromInput(r.job, job7)) {
        MessageBoxW(gDoc.hwnd,
            L"Job No must be either:\n\n"
            L"  - 8 digits (########)\n"
            L"  - B00#### (7 chars)\n"
            L"  - 4 digits (####)\n",
            L"DOC", MB_ICONERROR);
        return false;
    }


    if (!ResolveProjectFolderFromJob7(job7, outPath)) return false;
    if (!DirectoryExists(outPath)) {
        std::wstring msg = L"Project folder not found:\n\n" + outPath;
        MessageBoxW(gDoc.hwnd, msg.c_str(), L"DOC", MB_ICONERROR);
        return false;
    }
    return true;
}


static void DocApplyRowHeight(HWND lv)
{
    if (gDoc.rowHeightIL) {
        ImageList_Destroy(gDoc.rowHeightIL);
        gDoc.rowHeightIL = nullptr;
    }


    HDC hdc = GetDC(lv);
    HFONT old = (HFONT)SelectObject(hdc, gUIFont);
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, old);
    ReleaseDC(lv, hdc);


    int rowH = std::max(26, (int)tm.tmHeight + 10);
    gDoc.rowHeightIL = ImageList_Create(1, rowH, ILC_COLOR32, 1, 1);
    if (gDoc.rowHeightIL) {
        ListView_SetImageList(lv, gDoc.rowHeightIL, LVSIL_SMALL);
    }
}


static void DocInitColumns(HWND lv)
{
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;


    col.fmt = LVCFMT_CENTER;
    col.pszText = (LPWSTR)L"Open Folder";
    col.cx = 90;
    ListView_InsertColumn(lv, DOC_COL_OPEN, &col);


    col.fmt = LVCFMT_LEFT;
    col.pszText = (LPWSTR)L"Job No.";
    col.cx = 140;
    ListView_InsertColumn(lv, DOC_COL_JOB, &col);


    col.pszText = (LPWSTR)L"Description";
    col.cx = 300;
    ListView_InsertColumn(lv, DOC_COL_DESC, &col);


    col.pszText = (LPWSTR)L"Scope";
    col.cx = 220;
    ListView_InsertColumn(lv, DOC_COL_SCOPE, &col);


    col.pszText = (LPWSTR)L"Due";
    col.cx = 110;
    ListView_InsertColumn(lv, DOC_COL_DUE, &col);


    col.fmt = LVCFMT_CENTER;
    col.pszText = (LPWSTR)L"Status";
    col.cx = 70;
    ListView_InsertColumn(lv, DOC_COL_STAT, &col);


    col.pszText = (LPWSTR)L"Delete";
    col.cx = 70;
    ListView_InsertColumn(lv, DOC_COL_DEL, &col);
}


static void DocGetFilters(std::wstring& outStatus, std::wstring& outText)
{
    outStatus.clear();
    outText.clear();


    if (gDoc.editStatus) {
        wchar_t b[128]{};
        GetWindowTextW(gDoc.editStatus, b, _countof(b));
        outStatus = TrimSpaces(b);
    }
    if (gDoc.editText) {
        wchar_t b[512]{};
        GetWindowTextW(gDoc.editText, b, _countof(b));
        outText = TrimSpaces(b);
    }
}


static bool DocFilterMatch(const ProjectRow& r, const std::wstring& statusFilter, const std::wstring& textFilter)
{
    if (!statusFilter.empty()) {
        int want = _wtoi(statusFilter.c_str());
        if (want > 0 && r.status != want) return false;
    }


    if (!textFilter.empty()) {
        std::wstring hay = r.job + L" " + r.desc + L" " + r.scope;
        if (!ContainsI(hay, textFilter)) return false;
    }


    return true;
}


// ListView item lParam stores model index
static int DocModelIndexFromViewRow(int viewRow)
{
    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = viewRow;
    if (!ListView_GetItem(gDoc.lv, &it)) return -1;
    return (int)it.lParam;
}


static void DocAutofillDescFromFolder_ForViewRow(int viewRow)
{
    int modelIndex = DocModelIndexFromViewRow(viewRow);
    MessageBoxW(gDoc.hwnd, L"Autofill triggered", L"DOC", MB_OK);
    if (modelIndex < 0 || modelIndex >= (int)gDoc.rows.size()) return;


    ProjectRow& r = gDoc.rows[modelIndex];


    std::wstring base = GetActiveBasePath();
    if (base.empty() || !DirectoryExists(base)) return;


    // -------- 8-digit: ######## - Name --------
    std::wstring num8;
    if (TryParse8Digits(r.job, num8)) {
        std::wstring projPath;
        if (!ResolveTopLevelBy8DigitPrefix(base, num8, projPath) || !DirectoryExists(projPath))
            return;


        std::wstring folderName = LeafNameFromPath(projPath);


        std::wstring remainder;
        if (folderName.size() >= 8 && folderName.compare(0, 8, num8) == 0)
            remainder = folderName.substr(8);
        else
            remainder = folderName;


        remainder = TrimAfterIdSeparators(remainder);


        if (!remainder.empty())
            r.desc = remainder;


        r.job = num8; // normalize
        return;
    }


    // -------- B00#### / 4 digits --------
    std::wstring job7;
    if (!ParseJob7FromInput(r.job, job7)) return;


    std::wstring projPath;
    if (!ResolveProjectFolderFromJob7(job7, projPath) || !DirectoryExists(projPath))
        return;


    std::wstring folderName = LeafNameFromPath(projPath);


    std::wstring remainder;
    if (folderName.size() >= 7 && _wcsnicmp(folderName.c_str(), job7.c_str(), 7) == 0)
        remainder = folderName.substr(7);
    else
        remainder = folderName;


    remainder = TrimAfterIdSeparators(remainder);


    if (!remainder.empty())
        r.desc = remainder;


    r.job = job7; // normalize
}


static void DocRefreshList()
{
    if (!gDoc.lv) return;


    std::wstring statusFilter, textFilter;
    DocGetFilters(statusFilter, textFilter);


    ListView_DeleteAllItems(gDoc.lv);


    for (int i = 0; i < (int)gDoc.rows.size(); ++i) {
        const auto& r = gDoc.rows[i];
        if (!DocFilterMatch(r, statusFilter, textFilter)) continue;


        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = ListView_GetItemCount(gDoc.lv);
        it.iSubItem = 0;
        it.lParam = (LPARAM)i;


        it.pszText = (LPWSTR)L"\uE8B7"; // folder icon
        int row = ListView_InsertItem(gDoc.lv, &it);


        ListView_SetItemText(gDoc.lv, row, DOC_COL_JOB, (LPWSTR)r.job.c_str());
        ListView_SetItemText(gDoc.lv, row, DOC_COL_DESC, (LPWSTR)r.desc.c_str());
        ListView_SetItemText(gDoc.lv, row, DOC_COL_SCOPE, (LPWSTR)r.scope.c_str());


        std::wstring due = r.hasDue ? FormatDue_DDMMYYYY_Dots(r.due) : L"";
        ListView_SetItemText(gDoc.lv, row, DOC_COL_DUE, (LPWSTR)due.c_str());


        wchar_t sbuf[32]{};
        swprintf_s(sbuf, L"%d", r.status);
        ListView_SetItemText(gDoc.lv, row, DOC_COL_STAT, sbuf);


        ListView_SetItemText(gDoc.lv, row, DOC_COL_DEL, (LPWSTR)L"\uE74D"); // delete icon
    }
}


static void DocAddEmptyRow()
{
    ProjectRow r{};
    r.status = 1;
    gDoc.rows.push_back(r);
    DocRefreshList();


    int viewCount = ListView_GetItemCount(gDoc.lv);
    if (viewCount > 0) {
        ListView_EnsureVisible(gDoc.lv, viewCount - 1, FALSE);
    }
}


static void DocRemoveByModelIndex(int modelIndex)
{
    if (modelIndex < 0 || modelIndex >= (int)gDoc.rows.size()) return;
    gDoc.rows.erase(gDoc.rows.begin() + modelIndex);
    DocRefreshList();
}


// Overlay editing
static void DocHideOverlay()
{
    if (gDoc.overlay) ShowWindow(gDoc.overlay, SW_HIDE);
    gDoc.editViewRow = -1;
    gDoc.editCol = -1;
}


static void DocCommitOverlay(bool cancel)
{
    if (gDoc.editViewRow < 0 || gDoc.editCol < 0) { DocHideOverlay(); return; }


    int modelIndex = DocModelIndexFromViewRow(gDoc.editViewRow);
    if (modelIndex < 0) { DocHideOverlay(); return; }
    if (modelIndex >= (int)gDoc.rows.size()) { DocHideOverlay(); return; }


    if (!cancel) {
        wchar_t buf[2048]{};
        GetWindowTextW(gDoc.overlay, buf, _countof(buf));
        std::wstring v = TrimSpaces(buf);


        ProjectRow& r = gDoc.rows[modelIndex];


        if (gDoc.editCol == DOC_COL_JOB) r.job = v;
        if (gDoc.editCol == DOC_COL_DESC) r.desc = v;
        if (gDoc.editCol == DOC_COL_SCOPE) r.scope = v;


        if (gDoc.editCol == DOC_COL_STAT) {
            int s = _wtoi(v.c_str());
            r.status = (s <= 0) ? 1 : s;
        }


        if (gDoc.editCol == DOC_COL_DUE) {
            if (v.empty()) {
                r.hasDue = false;
                ZeroMemory(&r.due, sizeof(r.due));
            }
            else {
                SYSTEMTIME st{};
                if (!ParseDueFlexible(v, st)) {
                    MessageBoxW(gDoc.hwnd,
                        L"Invalid due date.\n\nType as:\n  ddmmyyyy  (8 digits)\nOR\n  dd.mm.yyyy",
                        L"Due date", MB_ICONERROR);
                    SetFocus(gDoc.overlay);
                    SendMessageW(gDoc.overlay, EM_SETSEL, 0, -1);
                    return;
                }
                r.hasDue = true;
                r.due = st;
            }
        }
    }


    DocHideOverlay();
    DocRefreshList();
}
static void DocAutofillDescFromFolder_ForViewRow(int viewRow);


static LRESULT CALLBACK DocOverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) { DocCommitOverlay(false); return 0; }
        if (wParam == VK_ESCAPE) { DocCommitOverlay(true); return 0; }
        if (wParam == VK_TAB) {
            int viewRow = gDoc.editViewRow;
            int col = gDoc.editCol;


            DocCommitOverlay(false);


            if (col == DOC_COL_JOB) {
                DocAutofillDescFromFolder_ForViewRow(viewRow);
                DocRefreshList();
            }


            int next = -1;
            if (col == DOC_COL_JOB) next = DOC_COL_DESC;
            else if (col == DOC_COL_DESC) next = DOC_COL_SCOPE;
            else if (col == DOC_COL_SCOPE) next = DOC_COL_DUE;
            else if (col == DOC_COL_DUE) next = DOC_COL_STAT;
            else if (col == DOC_COL_STAT) next = DOC_COL_JOB;


            if (next >= 0) {
                PostMessageW(gDoc.hwnd, WM_APP + 10, (WPARAM)viewRow, (LPARAM)next);
            }
            return 0;
        }


    }


    return CallWindowProcW(gOldDocOverlayProc, hwnd, msg, wParam, lParam);
}


static void DocStartEditCell(int viewRow, int col)
{
    if (!gDoc.lv || !gDoc.overlay) return;


    int modelIndex = DocModelIndexFromViewRow(viewRow);
    if (modelIndex < 0 || modelIndex >= (int)gDoc.rows.size()) return;


    // editable columns only
    if (!(col == DOC_COL_JOB || col == DOC_COL_DESC || col == DOC_COL_SCOPE || col == DOC_COL_DUE || col == DOC_COL_STAT))
        return;


    RECT rc{};
    ListView_GetSubItemRect(gDoc.lv, viewRow, col, LVIR_BOUNDS, &rc);


    POINT tl{ rc.left, rc.top };
    ClientToScreen(gDoc.lv, &tl);
    ScreenToClient(gDoc.hwnd, &tl);


    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;


    gDoc.editViewRow = viewRow;
    gDoc.editCol = col;


    const ProjectRow& r = gDoc.rows[modelIndex];
    std::wstring initial;


    if (col == DOC_COL_JOB) initial = r.job;
    if (col == DOC_COL_DESC) initial = r.desc;
    if (col == DOC_COL_SCOPE) initial = r.scope;
    if (col == DOC_COL_STAT) initial = std::to_wstring(r.status);
    if (col == DOC_COL_DUE) initial = r.hasDue ? FormatDue_DDMMYYYY_Dots(r.due) : L"";


    if (col == DOC_COL_DUE) {
        SendMessageW(gDoc.overlay, EM_SETCUEBANNER, TRUE, (LPARAM)L"ddmmyyyy or dd.mm.yyyy");
        SendMessageW(gDoc.overlay, EM_SETLIMITTEXT, 16, 0);
    }
    else {
        SendMessageW(gDoc.overlay, EM_SETCUEBANNER, TRUE, (LPARAM)L"");
        SendMessageW(gDoc.overlay, EM_SETLIMITTEXT, 2047, 0);
    }


    SetWindowTextW(gDoc.overlay, initial.c_str());
    SetWindowPos(gDoc.overlay, nullptr, tl.x, tl.y, w, h, SWP_NOZORDER | SWP_SHOWWINDOW);
    SetFocus(gDoc.overlay);
    SendMessageW(gDoc.overlay, EM_SETSEL, 0, -1);
}


static void DocLayout(HWND hwnd)
{
    RECT r{}; GetClientRect(hwnd, &r);
    const int pad = 12;
    const int bottomBarH = 44;


    RECT listRc = r;
    listRc.left += pad;
    listRc.right -= pad;
    listRc.top += pad;
    listRc.bottom -= (bottomBarH + pad);


    if (gDoc.lv) {
        SetWindowPos(gDoc.lv, nullptr, listRc.left, listRc.top,
            listRc.right - listRc.left, listRc.bottom - listRc.top,
            SWP_NOZORDER);
    }


    int y = r.bottom - bottomBarH + 8;


    if (gDoc.btnAdd) SetWindowPos(gDoc.btnAdd, nullptr, pad, y, 90, 26, SWP_NOZORDER);
    if (gDoc.btnSave) SetWindowPos(gDoc.btnSave, nullptr, pad + 100, y, 70, 26, SWP_NOZORDER);


    HWND lblStatus = GetDlgItem(hwnd, 9001);
    HWND lblText = GetDlgItem(hwnd, 9002);


    if (lblStatus) SetWindowPos(lblStatus, nullptr, pad + 190, y + 4, 60, 18, SWP_NOZORDER);
    if (gDoc.editStatus) SetWindowPos(gDoc.editStatus, nullptr, pad + 250, y, 50, 26, SWP_NOZORDER);


    if (lblText) SetWindowPos(lblText, nullptr, pad + 310, y + 4, 60, 18, SWP_NOZORDER);
    if (gDoc.editText) SetWindowPos(gDoc.editText, nullptr, pad + 370, y, 280, 26, SWP_NOZORDER);
}


static LRESULT CALLBACK DocListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        gDoc.hwnd = hwnd;


        gDoc.lv = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEWW,
            L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 10, 10,
            hwnd,
            (HMENU)(INT_PTR)ID_DOC_LISTVIEW,
            GetModuleHandleW(nullptr),
            nullptr
        );


        // ✅ Styling once
        ApplyFont(gDoc.lv, gUIFont);
        ListView_SetExtendedListViewStyle(
            gDoc.lv,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER
        );


        DocInitColumns(gDoc.lv);
        DocApplyRowHeight(gDoc.lv);




        gDoc.btnAdd = CreateWindowW(L"BUTTON", L"Add Line",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_DOC_ADDLINE, nullptr, nullptr);
        ApplyFont(gDoc.btnAdd, gUIFont);


        gDoc.btnSave = CreateWindowW(L"BUTTON", L"Save",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_DOC_SAVE, nullptr, nullptr);
        ApplyFont(gDoc.btnSave, gUIFont);


        CreateWindowW(L"STATIC", L"Status:",
            WS_CHILD | WS_VISIBLE,
            0, 0, 10, 10, hwnd, (HMENU)9001, nullptr, nullptr);


        gDoc.editStatus = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_DOC_FILTER_STATUS, nullptr, nullptr);
        ApplyFont(gDoc.editStatus, gUIFont);


        CreateWindowW(L"STATIC", L"Search:",
            WS_CHILD | WS_VISIBLE,
            0, 0, 10, 10, hwnd, (HMENU)9002, nullptr, nullptr);


        gDoc.editText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_DOC_FILTER_TEXT, nullptr, nullptr);
        ApplyFont(gDoc.editText, gUIFont);


        // Overlay edit (white background via WM_CTLCOLOREDIT in DocProc)
        gDoc.overlay = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_AUTOHSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_DOC_OVERLAY_EDIT, nullptr, nullptr);
        ApplyFont(gDoc.overlay, gUIFont);
        ShowWindow(gDoc.overlay, SW_HIDE);


        gOldDocOverlayProc = (WNDPROC)SetWindowLongPtrW(gDoc.overlay, GWLP_WNDPROC, (LONG_PTR)DocOverlayProc);


        // Load + refresh
        DocLoadFromDisk();
        DocRefreshList();


        return 0;
    }


    case WM_SIZE:
        DocLayout(hwnd);
        return 0;


    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT r; GetClientRect(hwnd, &r);
        FillRect(hdc, &r, gBgBrush);
        return TRUE;
    }


    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, gSettings.fg);
        SetBkMode(hdc, TRANSPARENT);
        return (INT_PTR)gBgBrush;
    }


    case WM_CTLCOLOREDIT:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, OPAQUE);
        return (INT_PTR)(gEditWhiteBrush ? gEditWhiteBrush : (HBRUSH)GetStockObject(WHITE_BRUSH));
    }


    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
        if (!dis || dis->CtlType != ODT_BUTTON) break;


        wchar_t text[128]{};
        GetWindowTextW(dis->hwndItem, text, _countof(text));


        bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        bool disabled = (dis->itemState & ODS_DISABLED) != 0;
        bool focused = (dis->itemState & ODS_FOCUS) != 0;


        DrawFancyIconTile(dis->hDC, dis->rcItem, text, gUIFont, false, pressed, disabled, focused);
        return TRUE;
    }


    case WM_APP + 10:
        DocStartEditCell((int)wParam, (int)lParam);
        return 0;


    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);


        if (id == ID_DOC_ADDLINE && code == BN_CLICKED) {
            DocAddEmptyRow();
            return 0;
        }


        if (id == ID_DOC_SAVE && code == BN_CLICKED) {
            DocSaveToDisk();
            MessageBoxW(hwnd, L"Saved.", L"DOC", MB_OK);
            return 0;
        }


        if ((id == ID_DOC_FILTER_STATUS || id == ID_DOC_FILTER_TEXT) && code == EN_CHANGE) {
            DocRefreshList();
            return 0;
        }


        return 0;
    }
    case WM_NOTIFY:
    {
        LPNMHDR hdr = (LPNMHDR)lParam;
        if (!hdr) break;


        // 1) Custom draw for DOC list view
        if (hdr->hwndFrom == gDoc.lv && hdr->code == NM_CUSTOMDRAW)
        {
            auto* cd = (LPNMLVCUSTOMDRAW)lParam;


            switch (cd->nmcd.dwDrawStage)
            {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;


            case CDDS_ITEMPREPAINT:
                return CDRF_NOTIFYSUBITEMDRAW;


            case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
            {
                int viewRow = (int)cd->nmcd.dwItemSpec;
                int modelIndex = DocModelIndexFromViewRow(viewRow);


                int status = 5;
                if (modelIndex >= 0 && modelIndex < (int)gDoc.rows.size())
                    status = gDoc.rows[modelIndex].status;


                switch (status)
                {
                case 1: cd->clrTextBk = RGB(255, 180, 180); break;
                case 2: cd->clrTextBk = RGB(180, 200, 255); break;
                case 3: cd->clrTextBk = RGB(180, 255, 200); break;
                case 4: cd->clrTextBk = RGB(255, 255, 180); break;
                default: cd->clrTextBk = RGB(255, 255, 255); break;
                }
                cd->clrText = RGB(0, 0, 0);


                return CDRF_DODEFAULT;
            }
            }
            return CDRF_DODEFAULT;
        }


        // 2) Double-click handling (OPEN / DELETE / EDIT)
        if (hdr->hwndFrom == gDoc.lv && hdr->code == NM_DBLCLK)
        {
            auto* act = (LPNMITEMACTIVATE)lParam;
            if (act->iItem >= 0 && act->iSubItem >= 0)
            {
                if (act->iSubItem == DOC_COL_OPEN)
                {
                    int modelIndex = DocModelIndexFromViewRow(act->iItem);
                    if (modelIndex >= 0 && modelIndex < (int)gDoc.rows.size())
                    {
                        std::wstring path;
                        if (DocResolveRowToPath(gDoc.rows[modelIndex], path))
                        {
                            OpenFolderInExplorer(path);
                            AddRecentFolder(path);
                        }
                    }
                    return 0;
                }


                if (act->iSubItem == DOC_COL_DEL)
                {
                    int modelIndex = DocModelIndexFromViewRow(act->iItem);
                    if (modelIndex >= 0)
                    {
                        DocRemoveByModelIndex(modelIndex);
                        DocSaveToDisk();
                    }
                    return 0;
                }


                DocStartEditCell(act->iItem, act->iSubItem);
                return 0;
            }
            return 0;
        }


        break;
    }


    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;


    case WM_DESTROY:
        DocSaveToDisk();
        if (gDoc.rowHeightIL) { ImageList_Destroy(gDoc.rowHeightIL); gDoc.rowHeightIL = nullptr; }
        gDoc = DocState{};
        return 0;
    }


    return DefWindowProcW(hwnd, msg, wParam, lParam);
}


static void OpenDocListWindow(HWND parent)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DocListProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kDocListClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }


    HWND wnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kDocListClass,
        L"Project List",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        880, 520,
        parent, nullptr, GetModuleHandleW(nullptr),
        nullptr);


    if (!wnd) {
        MessageBoxW(parent, L"Failed to open DOC window.", L"Error", MB_ICONERROR);
        return;
    }


    ShowWindow(wnd, SW_SHOW);
    UpdateWindow(wnd);
}


// -------------------- Font creation --------------------


static void CreateIconFont()
{
    LOGFONTW lf{};
    lf.lfHeight = -20;
    wcscpy_s(lf.lfFaceName, L"Segoe Fluent Icons");
    HFONT fTry = CreateFontIndirectW(&lf);


    bool ok = false;
    if (fTry) {
        HDC hdc = GetDC(nullptr);
        HFONT old = (HFONT)SelectObject(hdc, fTry);
        wchar_t face[LF_FACESIZE]{};
        GetTextFaceW(hdc, LF_FACESIZE, face);
        SelectObject(hdc, old);
        ReleaseDC(nullptr, hdc);
        ok = (_wcsicmp(face, L"Segoe Fluent Icons") == 0);
    }


    if (!ok) {
        if (fTry) DeleteObject(fTry);
        wcscpy_s(lf.lfFaceName, L"Segoe MDL2 Assets");
        gIconFont = CreateFontIndirectW(&lf);
    }
    else {
        gIconFont = fTry;
    }
}


static HWND CreateIconButton(HWND parent, int id, const wchar_t* glyph)
{
    HWND b = CreateWindowW(L"BUTTON", glyph,
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 10, 10,
        parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    ApplyFont(b, gIconFont);
    return b;
}


// -------------------- Main proc --------------------


static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        gMainHwnd = hwnd;

        gUIFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        CreateIconFont();


        LOGFONTW lf{};
        GetObjectW(gUIFont, sizeof(lf), &lf);
        lf.lfHeight = -16;
        gMenuFont = CreateFontIndirectW(&lf);


        RebuildBrush();


        gBtnFlag = CreateIconButton(hwnd, ID_BTN_FLAG, L"\uE7C1");
        gBtnFolder = CreateIconButton(hwnd, ID_BTN_FOLDER, L"\uE8B7");


        // New Project button: "Add" glyph
        gBtnNewProject = CreateIconButton(hwnd, ID_BTN_NEWPROJECT, L"\uE710");


        gBtnDoc = CreateIconButton(hwnd, ID_BTN_DOC, L"\uE8A5");
        gBtnCRM = nullptr; // CRM icon removed (entry point is Phone button)
        gBtnMail = CreateIconButton(hwnd, ID_BTN_MAIL, L"\uE715");
        gBtnCal = CreateIconButton(hwnd, ID_BTN_CAL, L"\uE787");
        gBtnPhone = CreateIconButton(hwnd, ID_BTN_PHONE, L"\uE717");
        gBtnGear = CreateIconButton(hwnd, ID_BTN_GEAR, L"\uE713");
        gBtnMin = CreateIconButton(hwnd, ID_BTN_MIN, L"\uE921"); // minimize icon
        gBtnQuit = CreateIconButton(hwnd, ID_BTN_QUIT, L"\uE8BB"); // Power / Close icon


        gEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 10, 10,
            hwnd, (HMENU)(INT_PTR)ID_EDIT, nullptr, nullptr);
        ApplyFont(gEdit, gUIFont);


        gOldEditProc = (WNDPROC)SetWindowLongPtrW(gEdit, GWLP_WNDPROC, (LONG_PTR)EditProc);


        LayoutMainBar();
        ApplySearchModeToEdit();
        SetFocus(gEdit);
        return 0;
    }


    case WM_MEASUREITEM:
    {
        auto* mi = (MEASUREITEMSTRUCT*)lParam;
        if (mi && mi->CtlType == ODT_MENU) { MeasureThemedMenuItem(mi); return TRUE; }
        break;
    }


    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
        if (!dis) break;
        if (dis->CtlType == ODT_BUTTON) { DrawFancyIconButton(dis); return TRUE; }
        if (dis->CtlType == ODT_MENU) { DrawThemedMenuItem(dis); return TRUE; }
        break;
    }


    case WM_MOUSEMOVE:
    {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        HWND hChild = ChildWindowFromPointEx(hwnd, pt, CWP_SKIPINVISIBLE);


        HWND newHover = nullptr;
        if (hChild == gBtnFlag || hChild == gBtnFolder || hChild == gBtnNewProject ||
            hChild == gBtnDoc || hChild == gBtnMail || hChild == gBtnCal ||
            hChild == gBtnPhone || hChild == gBtnGear)
            newHover = hChild;


        if (newHover != gHoverBtn) {
            HWND old = gHoverBtn;
            gHoverBtn = newHover;
            if (old) InvalidateRect(old, nullptr, TRUE);
            if (gHoverBtn) InvalidateRect(gHoverBtn, nullptr, TRUE);
        }
        BeginTrackMouseLeave();
        return 0;
    }


    case WM_MOUSELEAVE:
        gTrackingMouseLeave = false;
        if (gHoverBtn) {
            HWND old = gHoverBtn;
            gHoverBtn = nullptr;
            InvalidateRect(old, nullptr, TRUE);
        }
        return 0;


    case WM_APP + 501:
        // Silent update check on startup
        CheckForUpdates_Ask(hwnd, true);
        return 0;


    case WM_APP_OPEN_SETTINGS:
        OpenSettingsDialog(hwnd);
        return 0;

    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);

        const bool isToolbarBtn =
            id == ID_BTN_FLAG || id == ID_BTN_FOLDER || id == ID_BTN_NEWPROJECT ||
            id == ID_BTN_DOC || id == ID_BTN_MAIL || id == ID_BTN_CAL ||
            id == ID_BTN_PHONE || id == ID_BTN_GEAR || id == ID_BTN_MIN ||
            id == ID_BTN_QUIT;

        // Only act on real clicks (prevents focus/mouse notifications)
        if (isToolbarBtn && code != BN_CLICKED)
            return 0;

        if (id == ID_BTN_MIN) { ShowWindow(hwnd, SW_MINIMIZE); return 0; }

        if (id == ID_BTN_QUIT) {
            int r = MessageBoxW(hwnd,
                L"This will close the application.\n\nAre you sure?",
                L"Quit",
                MB_ICONQUESTION | MB_YESNO);

            if (r == IDYES) {
                DestroyWindow(hwnd); // triggers WM_DESTROY
            }
            return 0;
        }

        if (id == ID_BTN_GEAR) { PostMessageW(hwnd, WM_APP_OPEN_SETTINGS, 0, 0); return 0; }
        if (id == ID_BTN_CAL) { OutlookOpenCalendar(); return 0; }
        if (id == ID_BTN_FOLDER) { ShowFolderDropdown(); return 0; }
        if (id == ID_BTN_FLAG) { ShowRecentDropdown(); return 0; }
        if (id == ID_BTN_DOC) { OpenDocListWindow(hwnd); return 0; }
        if (id == ID_BTN_PHONE) { ProjectCrmSearchDialog(hwnd); return 0; }
        if (id == ID_BTN_MAIL) { ShellExecuteW(nullptr, L"open", L"outlook.exe", nullptr, nullptr, SW_SHOWNORMAL); return 0; }

        // NEW: Create project from template
        if (id == ID_BTN_NEWPROJECT) { CreateNewProjectFromTemplate(); return 0; }

        return 0;
    }

    case WM_CONTEXTMENU:
    {
        HWND hCtrl = (HWND)wParam;
        if (hCtrl == gBtnCal) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (pt.x == -1 && pt.y == -1) {
                RECT r{}; GetWindowRect(gBtnCal, &r);
                pt.x = r.left; pt.y = r.bottom;
            }
            ShowCalendarContextMenu(hwnd, pt);
            return 0;
        }
        break;
    }


    // Drag borderless window by empty background.
    case WM_NCHITTEST:
    {
        LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
        if (hit == HTCLIENT) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            HWND hChild = ChildWindowFromPointEx(hwnd, pt, CWP_SKIPINVISIBLE);
            if (hChild && hChild != hwnd) return HTCLIENT;
            return HTCAPTION;
        }
        return hit;
    }


    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT r{}; GetClientRect(hwnd, &r);
        FillRect(hdc, &r, gBgBrush ? gBgBrush : (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }


    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, gSettings.fg);
        SetBkMode(hdc, TRANSPARENT);
        return (INT_PTR)(gBgBrush ? gBgBrush : (HBRUSH)GetStockObject(BLACK_BRUSH));
    }


    // white main edit
    case WM_CTLCOLOREDIT:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, OPAQUE);
        return (INT_PTR)(gEditWhiteBrush ? gEditWhiteBrush : (HBRUSH)GetStockObject(WHITE_BRUSH));
    }


    case WM_DESTROY:
        if (gIconFont) DeleteObject(gIconFont);
        if (gMenuFont) DeleteObject(gMenuFont);
        if (gBgBrush) DeleteObject(gBgBrush);
        if (gMenuBgBrush) DeleteObject(gMenuBgBrush);
        if (gSelBrush) DeleteObject(gSelBrush);
        if (gEditWhiteBrush) DeleteObject(gEditWhiteBrush);
        PostQuitMessage(0);
        return 0;
    }


    return DefWindowProcW(hwnd, msg, wParam, lParam);
}


// -------------------- wWinMain --------------------


int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);


    INITCOMMONCONTROLSEX icc{ sizeof(icc),
        (DWORD)(ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_LINK_CLASS) };
    InitCommonControlsEx(&icc);


    LoadUiSettings();


    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kMainClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);


    if (!RegisterClassW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register main window class.", L"Error", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }


    // Borderless bar.
    HWND hwnd = CreateWindowExW(
        0, kMainClassName, kTitle,
        WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT,
        350, 46,
        nullptr, nullptr, hInstance, nullptr);


    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create window.", L"Error", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    auto SettingsMissing = [&]() -> bool
        {
            return TrimSpaces(gSettings.baseOverride).empty()
                || TrimSpaces(gSettings.templatePath).empty()
                || TrimSpaces(gSettings.crmFolderPath).empty()
                || TrimSpaces(gSettings.contractorCsvPath).empty();
        };

    // Force user to enter settings on first run (or if missing)
    if (SettingsMissing())
    {
        // Keep main window hidden until settings are valid
        ShowWindow(hwnd, SW_HIDE);

        MessageBoxW(hwnd,
            L"Before using Project Finder, you must complete the Settings.\n\n"
            L"Please set Base Path, Template Folder, CRM Folder Path, and Contractor Contact List.csv.",
            L"Settings Required",
            MB_OK | MB_ICONINFORMATION);

        OpenSettingsDialog(hwnd);   // your function already runs a modal loop

        // If still missing after closing settings, quit
        if (SettingsMissing())
        {
            MessageBoxW(hwnd,
                L"Settings are required to continue.\n\nThe application will now close.",
                L"Settings Required",
                MB_OK | MB_ICONERROR);

            DestroyWindow(hwnd);
            CoUninitialize();
            return 0;
        }
    }


    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Silent update check on startup (no prompt if already up to date)
    PostMessageW(hwnd, WM_APP + 501, 0, 0);


    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }


    CoUninitialize();
    return 0;
}