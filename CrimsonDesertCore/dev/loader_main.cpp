/**
 * @file loader_main.cpp
 * @brief Hot-reload loader stub shared by all CrimsonDesert mod dev builds.
 *
 * Loads the configured logic DLL and polls Numpad 0 to unload/reload it
 * in-place. No DetourModKit dependency — logging via OutputDebugStringA
 * only so this stays a thin stub.
 *
 * Parameterized by three macros the consumer's CMakeLists.txt is expected
 * to supply via target_compile_definitions:
 *
 *   CDCORE_LOADER_DLL_NAME   : filename of the logic DLL to load,
 *                              e.g. "CrimsonDesertLiveTransmog_Logic.dll"
 *   CDCORE_LOADER_PDB_NAME   : companion PDB to move from staging/,
 *                              e.g. "CrimsonDesertLiveTransmog_Logic.pdb"
 *   CDCORE_LOADER_LOG_PREFIX : debug-log prefix string,
 *                              e.g. "[TransmogLoader] "
 *
 * These are strings (with quotes) baked at compile time; the loader holds
 * no game-specific knowledge beyond them.
 */

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

// IntelliSense parses this file from the CDCore workspace without a
// consumer's compile_commands entry, so the macros aren't defined during
// that standalone parse. Stub them under __INTELLISENSE__ so the linter
// doesn't cascade-error on every reference below. The `#error` still
// fires on a real compile where the consumer must supply the macros.
#if defined(__INTELLISENSE__)
#  ifndef CDCORE_LOADER_DLL_NAME
#    define CDCORE_LOADER_DLL_NAME "stub_logic.dll"
#  endif
#  ifndef CDCORE_LOADER_PDB_NAME
#    define CDCORE_LOADER_PDB_NAME "stub_logic.pdb"
#  endif
#  ifndef CDCORE_LOADER_LOG_PREFIX
#    define CDCORE_LOADER_LOG_PREFIX "[StubLoader] "
#  endif
#else
#  ifndef CDCORE_LOADER_DLL_NAME
#    error "CDCORE_LOADER_DLL_NAME must be defined to the logic DLL filename (string)"
#  endif
#  ifndef CDCORE_LOADER_PDB_NAME
#    error "CDCORE_LOADER_PDB_NAME must be defined to the logic PDB filename (string)"
#  endif
#  ifndef CDCORE_LOADER_LOG_PREFIX
#    error "CDCORE_LOADER_LOG_PREFIX must be defined to a debug-log prefix (string)"
#  endif
#endif

static constexpr const char *k_logicDllName   = CDCORE_LOADER_DLL_NAME;
static constexpr const char *k_logicPdbName   = CDCORE_LOADER_PDB_NAME;
static constexpr const char *k_logPrefix      = CDCORE_LOADER_LOG_PREFIX;
static constexpr const char *k_stagingSubdir  = "staging";
static constexpr int         k_pollIntervalMs = 100;
static constexpr int         k_postShutdownMs = 100;
static constexpr int         k_preLoadDelayMs = 200;
static constexpr int         k_vkReload       = VK_NUMPAD0;

static std::atomic<bool> s_running{false};
static std::atomic<bool> s_reloading{false};
static HANDLE            s_thread = nullptr;
static HMODULE           s_logicDll = nullptr;

using InitFn     = bool(__cdecl *)();
using ShutdownFn = void(__cdecl *)();

static InitFn     s_fnInit     = nullptr;
static ShutdownFn s_fnShutdown = nullptr;

static void log_msg(const char *msg)
{
    char buf[512];
    const int len = snprintf(buf, sizeof(buf), "%s%s\n", k_logPrefix, msg);
    if (len > 0 && static_cast<size_t>(len) < sizeof(buf))
        OutputDebugStringA(buf);
    else
    {
        // Truncation fallback — keep the prefix so the message still
        // routes to the right filter in DbgView.
        char fallback[128];
        snprintf(fallback, sizeof(fallback),
                 "%s(message truncated)\n", k_logPrefix);
        OutputDebugStringA(fallback);
    }
}

static std::string get_loader_dir(HMODULE hSelf)
{
    char path[MAX_PATH];
    GetModuleFileNameA(hSelf, path, MAX_PATH);

    char *lastSlash = std::strrchr(path, '\\');
    if (lastSlash)
        *(lastSlash + 1) = '\0';

    return std::string(path);
}

static void move_staged_file(const std::string &stagingDir,
                             const std::string &loaderDir,
                             const char *filename)
{
    std::string src = stagingDir + filename;
    std::string dst = loaderDir + filename;

    if (GetFileAttributesA(src.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;

    CopyFileA(src.c_str(), dst.c_str(), FALSE);
    DeleteFileA(src.c_str());
}

static bool copy_from_staging(const std::string &loaderDir)
{
    std::string stagingDir = loaderDir + k_stagingSubdir + "\\";
    std::string stagingDll = stagingDir + k_logicDllName;

    if (GetFileAttributesA(stagingDll.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;

    if (!CopyFileA(stagingDll.c_str(), (loaderDir + k_logicDllName).c_str(), FALSE))
    {
        log_msg("Failed to copy DLL from staging");
        return false;
    }
    DeleteFileA(stagingDll.c_str());

    move_staged_file(stagingDir, loaderDir, k_logicPdbName);

    log_msg("Copied logic DLL from staging");
    return true;
}

// --- Logic DLL lifecycle ---
static bool load_logic(const std::string &dllPath)
{
    s_logicDll = LoadLibraryA(dllPath.c_str());
    if (!s_logicDll)
    {
        char err[128];
        snprintf(err, sizeof(err), "LoadLibrary failed (error %lu)", GetLastError());
        log_msg(err);
        return false;
    }

    s_fnInit = reinterpret_cast<InitFn>(GetProcAddress(s_logicDll, "Init"));
    s_fnShutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(s_logicDll, "Shutdown"));

    if (!s_fnInit || !s_fnShutdown)
    {
        log_msg("FAILED to resolve Init/Shutdown exports");
        FreeLibrary(s_logicDll);
        s_logicDll = nullptr;
        s_fnInit = nullptr;
        s_fnShutdown = nullptr;
        return false;
    }

    if (!s_fnInit())
    {
        log_msg("Init() returned false");
        FreeLibrary(s_logicDll);
        s_logicDll = nullptr;
        s_fnInit = nullptr;
        s_fnShutdown = nullptr;
        return false;
    }

    log_msg("Logic DLL loaded and initialized");
    return true;
}

static void unload_logic()
{
    if (!s_logicDll)
        return;

    if (s_fnShutdown)
        s_fnShutdown();

    Sleep(k_postShutdownMs);

    FreeLibrary(s_logicDll);
    s_logicDll = nullptr;
    s_fnInit = nullptr;
    s_fnShutdown = nullptr;

    log_msg("Logic DLL unloaded");
}

// --- Loader thread ---
static DWORD WINAPI loader_thread(LPVOID param)
{
    HMODULE hSelf = static_cast<HMODULE>(param);
    std::string loaderDir = get_loader_dir(hSelf);
    std::string dllPath   = loaderDir + k_logicDllName;

    log_msg("Loader thread started");

    copy_from_staging(loaderDir);
    load_logic(dllPath);

    bool wasKeyDown = false;

    while (s_running.load(std::memory_order_relaxed))
    {
        Sleep(k_pollIntervalMs);

        bool isKeyDown = (GetAsyncKeyState(k_vkReload) & 0x8000) != 0;

        if (wasKeyDown && !isKeyDown)
        {
            if (!s_reloading.exchange(true, std::memory_order_acq_rel))
            {
                log_msg("Numpad0 released — reloading logic DLL...");

                unload_logic();

                Sleep(k_preLoadDelayMs);

                copy_from_staging(loaderDir);

                if (!load_logic(dllPath))
                    log_msg("Reload FAILED — logic DLL not loaded");

                s_reloading.store(false, std::memory_order_release);
            }
        }

        wasKeyDown = isKeyDown;
    }

    unload_logic();
    log_msg("Loader thread exiting");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);

        s_running.store(true, std::memory_order_release);
        s_thread = CreateThread(nullptr, 0, loader_thread, hModule, 0, nullptr);
        if (!s_thread)
        {
            s_running.store(false, std::memory_order_release);
            return FALSE;
        }
        break;

    case DLL_PROCESS_DETACH:
        s_running.store(false, std::memory_order_release);

        if (s_thread && lpReserved == nullptr)
        {
            WaitForSingleObject(s_thread, 5000);
            CloseHandle(s_thread);
            s_thread = nullptr;
        }
        break;

    default:
        break;
    }

    return TRUE;
}
