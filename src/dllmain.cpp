// Styx Engine Fix - standalone fixes for Styx: Master of Shadows (UE3, 64-bit), loaded as dinput8.dll.
//
// 1. Physics at high frame rates: UE3 steps PhysX once per frame with the frame time. PhysX pushes
//    interpenetrating bodies apart with a velocity of roughly depth / step, so at 360 fps a freshly started
//    ragdoll is pushed ~6x harder than at the 62 fps the game was tuned for and flies away. We hook
//    NpScene::setTiming and raise the step to at least 1/PhysicsRate; PhysX then accumulates frame times
//    and simulates in those fixed steps, independent of the frame rate.
// 2. Frame rate limit: the game caps itself at 62 fps ([Engine.Engine] MaxSmoothedFrameRate). Before the
//    engine reads its config we write the limit chosen in StyxEngineFix.ini into the user's StyxEngine.ini.
//
// Every other DirectInput call is forwarded to the system dinput8.dll.

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <string>

// ---------------------------------------------------------------- log

static std::wstring g_dir;  // game exe folder, with trailing backslash

static void Log(const char* fmt, ...)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, (g_dir + L"StyxEngineFix.log").c_str(), L"a") != 0 || !f)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02u:%02u:%02u ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------- settings

static int g_fpsLimit = 144;     // 0 = unlimited
static int g_physicsRate = 62;   // Hz; 0 = off (the game is tuned for its own 62 fps cap)

static void LoadSettings()
{
    const std::wstring ini = g_dir + L"StyxEngineFix.ini";
    g_fpsLimit = GetPrivateProfileIntW(L"Framerate", L"FpsLimit", 144, ini.c_str());
    g_physicsRate = GetPrivateProfileIntW(L"Physics", L"PhysicsRate", 62, ini.c_str());
    if (g_fpsLimit < 0)
        g_fpsLimit = 0;
    if (g_physicsRate < 0)
        g_physicsRate = 0;
}

// ---------------------------------------------------------------- frame rate limit

// %USERPROFILE%\Documents (or its redirected location, e.g. OneDrive) from the registry: shell32 must not
// be used while the loader lock is held.
static std::wstring DocumentsFolder()
{
    wchar_t raw[MAX_PATH] = {}, expanded[MAX_PATH] = {};
    DWORD size = sizeof(raw);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                     L"Personal", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, raw, &size) != ERROR_SUCCESS)
        return L"";
    ExpandEnvironmentStringsW(raw, expanded, MAX_PATH);
    return expanded;
}

static void ApplyFpsLimit()
{
    const std::wstring ini = DocumentsFolder() + L"\\My Games\\Styx\\StyxGame\\Config\\StyxEngine.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("frame rate: %ls not found (first start?), limit applies from the next start", ini.c_str());
        return;
    }
    const int limit = g_fpsLimit > 0 ? g_fpsLimit : 1000;  // the engine needs a number; 1000 = effectively none
    wchar_t value[16];
    swprintf_s(value, L"%d", limit);
    WritePrivateProfileStringW(L"Engine.Engine", L"bSmoothFrameRate", L"TRUE", ini.c_str());
    WritePrivateProfileStringW(L"Engine.Engine", L"MaxSmoothedFrameRate", value, ini.c_str());
    Log("frame rate: limit %s%d fps written to StyxEngine.ini", g_fpsLimit > 0 ? "" : "unlimited, ", limit);
}

// ---------------------------------------------------------------- physics step

using PFN_SetTiming = void(__fastcall*)(void* scene, float maxTimestep, uint32_t maxIter, int method);
static PFN_SetTiming g_origSetTiming = nullptr;
static void** g_setTimingSlot = nullptr;
static float g_minStep = 0.0f;

static void __fastcall HookSetTiming(void* scene, float maxTimestep, uint32_t maxIter, int method)
{
    if (method == 0 /* NX_TIMESTEP_FIXED */ && maxTimestep < g_minStep) {
        maxTimestep = g_minStep;
        if (maxIter < 8)
            maxIter = 8;  // enough steps when frames get slower again
    }
    g_origSetTiming(scene, maxTimestep, maxIter, method);
}

static bool WriteSlot(void** slot, void* value)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
        return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// Returns true when done (installed or not applicable).
static bool TryInstallPhysicsFix()
{
    HMODULE physx = GetModuleHandleW(L"PhysXCore64.dll");
    if (!physx)
        return false;
    // NpScene vtable slot 82 of the PhysXCore64.dll shipped with Styx; verify it points at NpScene::setTiming.
    const uintptr_t base = reinterpret_cast<uintptr_t>(physx);
    void** slot = reinterpret_cast<void**>(base + 0x31d758);
    if (reinterpret_cast<uintptr_t>(*slot) != base + 0x153890) {
        Log("physics: unknown PhysXCore64.dll build, fix not installed");
        return true;
    }
    g_origSetTiming = reinterpret_cast<PFN_SetTiming>(*slot);
    if (!WriteSlot(slot, reinterpret_cast<void*>(&HookSetTiming))) {
        Log("physics: VirtualProtect failed, fix not installed");
        return true;
    }
    g_setTimingSlot = slot;
    Log("physics: fixed step of at least 1/%d s installed", g_physicsRate);
    return true;
}

static DWORD WINAPI PhysicsWatcher(LPVOID)
{
    // PhysXCore64.dll is loaded by the engine during start-up; wait for it (up to 10 minutes).
    for (int i = 0; i < 6000; ++i) {
        if (TryInstallPhysicsFix())
            return 0;
        Sleep(100);
    }
    Log("physics: PhysXCore64.dll never loaded");
    return 0;
}

// ---------------------------------------------------------------- dinput8 forwarding

static HMODULE g_realDinput = nullptr;

static FARPROC Real(const char* name)
{
    if (!g_realDinput) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dinput8.dll");
        g_realDinput = LoadLibraryW(path);
    }
    return g_realDinput ? GetProcAddress(g_realDinput, name) : nullptr;
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto fn = reinterpret_cast<Fn>(Real("DirectInput8Create"));
    return fn ? fn(inst, version, riid, out, outer) : E_FAIL;
}

extern "C" HRESULT WINAPI DllCanUnloadNow()
{
    using Fn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<Fn>(Real("DllCanUnloadNow"));
    return fn ? fn() : S_FALSE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
{
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto fn = reinterpret_cast<Fn>(Real("DllGetClassObject"));
    return fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI DllRegisterServer()
{
    using Fn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<Fn>(Real("DllRegisterServer"));
    return fn ? fn() : E_FAIL;
}

extern "C" HRESULT WINAPI DllUnregisterServer()
{
    using Fn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<Fn>(Real("DllUnregisterServer"));
    return fn ? fn() : E_FAIL;
}

// ---------------------------------------------------------------- entry

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(module, path, MAX_PATH);
        g_dir = path;
        g_dir.resize(g_dir.find_last_of(L'\\') + 1);
        LoadSettings();
        Log("Styx Engine Fix loaded (fps limit %d, physics rate %d Hz)", g_fpsLimit, g_physicsRate);
        ApplyFpsLimit();
        if (g_physicsRate > 0) {
            g_minStep = 1.0f / static_cast<float>(g_physicsRate);
            if (HANDLE t = CreateThread(nullptr, 0, PhysicsWatcher, nullptr, 0, nullptr))
                CloseHandle(t);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_setTimingSlot)
            WriteSlot(g_setTimingSlot, reinterpret_cast<void*>(g_origSetTiming));
    }
    return TRUE;
}
