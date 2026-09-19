/**
 * @file mod_loader.cpp
 * @brief Resident dev loader shared by every Crimson Desert mod: owns the process, reloads one logic generation at a
 *        time.
 *
 * @details Structure follows DetourModKit's checked-in `examples/staged_reload` pair, which its hot-reload guide
 *          treats as the reference implementation. Three properties matter:
 *
 *          1. **Unique staged names.** Mapping the build output locks that path, so a rebuild cannot replace it and
 *             every reload replays identical bytes while reporting success. Mapping a REUSED name is worse: once an
 *             image is pinned, LoadLibrary on the same path silently returns the pinned predecessor. Each generation
 *             gets a name never used before in this process.
 *          2. **A resident wheel host.** A mouse-wheel binding makes the input engine take a permanent module
 *             keepalive on whichever module hosts wheel capture. Hosting it here - in a module that is never unloaded
 *             - is what lets a logic generation keep a user-bound wheel combo AND still unmap. This loader therefore
 *             links only DetourModKit::WheelHost, never the full archive.
 *          3. **Proof, not assumption.** A generation is only considered gone after its typed Shutdown accepts, a
 *             probe lease opens and closes, and one of its code addresses is observed to become unmapped.
 *
 *          This loader is dev-only. The release build is a single ASI with no reload path at all.
 *
 *          Parameterized by two macros the consumer's CMakeLists.txt supplies through
 *          target_compile_definitions. Every other file name derives from the first:
 *
 *            CDCORE_LOADER_MOD_NAME      : the mod's deployed stem, e.g. "CrimsonDesertLiveTransmog"
 *            CDCORE_LOADER_PROCESS_NAME  : the only process this loader runs in, e.g. "CrimsonDesert.exe"
 */

#include "loader_log.hpp"
#include "protocol.h"

#include <DetourModKit/abi/wheel_host.h>

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

// IntelliSense parses this file from the CDCore workspace without a consumer's compile_commands entry, so the macros
// are undefined during that standalone parse. Stub them there so the linter does not cascade-error on every reference
// below. The `#error` still fires on a real compile, where the consumer must supply them.
#if defined(__INTELLISENSE__)
#ifndef CDCORE_LOADER_MOD_NAME
#define CDCORE_LOADER_MOD_NAME "StubMod"
#endif
#ifndef CDCORE_LOADER_PROCESS_NAME
#define CDCORE_LOADER_PROCESS_NAME "StubGame.exe"
#endif
#else
#ifndef CDCORE_LOADER_MOD_NAME
#error "CDCORE_LOADER_MOD_NAME must be defined to the mod's deployed stem (string)"
#endif
#ifndef CDCORE_LOADER_PROCESS_NAME
#error "CDCORE_LOADER_PROCESS_NAME must be defined to the host process basename (string)"
#endif
#endif

namespace
{
    constexpr const char *MOD_NAME = CDCORE_LOADER_MOD_NAME;
    constexpr const char *PROCESS_NAME = CDCORE_LOADER_PROCESS_NAME;
    /**
     * @brief Stable suffix of a staged generation image.
     * @details Staged generations are "<mod>.genNNNN.logic.dll", so one glob covers the build output and every
     *          staged copy.
     */
    constexpr const char *GENERATION_SUFFIX = ".logic.dll";
    constexpr const char *STAGING_SUBDIR = "staging";

    constexpr int RELOAD_VK = VK_NUMPAD0;
    constexpr DWORD CONTROL_POLL_MS = 100;
    /// Quiescence so an in-flight per-frame detour body returns before FreeLibrary.
    constexpr DWORD POST_SHUTDOWN_MS = 100;
    constexpr DWORD UNMAP_POLL_MS = 10;
    /**
     * @brief Deadline for the post-FreeLibrary unmap poll.
     * @details A release can complete slightly after FreeLibrary returns, so the check polls rather than samples
     *          once. A single sample reports a healthy generation as pinned.
     */
    constexpr DWORD UNMAP_TIMEOUT_MS = 2000;

    /// Caps retained images before the loader stops reloading and asks for a restart.
    constexpr unsigned MAX_RETAINED_GENERATIONS = 24;

    /// Loader-owned owner id for the probe lease: ASCII "CDPROBE_". Any value a generation never uses.
    constexpr std::uint64_t LEASE_PROBE_OWNER = UINT64_C(0x434450524F42455F);

    std::atomic<bool> s_running{false};
    std::atomic<bool> s_reloading{false};
    HANDLE s_thread = nullptr;

    /// Process-lifetime wheel host. Started once, never stopped: the loader outlives every generation.
    WheelHostTable s_wheel_host{};
    std::uint64_t s_host_identity = 0;
    bool s_wheel_host_live = false;

    /// Latched when a generation cannot be proved gone. A further reload stacks unknown state.
    bool s_restart_required = false;
    unsigned s_generation_counter = 0;
    unsigned s_retained_generations = 0;

    using InitFn = unsigned(__cdecl *)(const CdReloadInitRequest *) noexcept;
    using ShutdownFn = unsigned(__cdecl *)() noexcept;
    using RevisionFn = const char *(__cdecl *)() noexcept;

    /// One mapped generation and everything needed to retire it.
    struct Generation
    {
        HMODULE module = nullptr;
        InitFn init = nullptr;
        ShutdownFn shutdown = nullptr;
        RevisionFn revision = nullptr;
        /// An address inside the image, used to prove the unmap. Any exported code address works.
        const void *unmap_address = nullptr;
        std::uint64_t generation_id = 0;
        std::string path;
    };

    std::optional<Generation> s_current;
    char s_log_path[MAX_PATH]{};
    std::string s_loader_dir;
    std::string s_log_prefix;
    std::string s_logic_dll_name;
    std::string s_logic_pdb_name;
    std::string s_generation_prefix;

    // Logging.

    /// Writes one line to the debugger and to the loader's own log file.
    void log_msg(const char *msg) noexcept
    {
        char line[768];
        if (std::snprintf(line, sizeof(line), "%s%s", s_log_prefix.c_str(), msg) <= 0)
        {
            return;
        }

        // The loader keeps its OWN log. The mod's log belongs to a Session that dies with each generation, and the
        // loader's most important lines are emitted while no Session exists at all.
        if (s_log_path[0] == '\0')
        {
            CDCore::dev::echo_to_debugger(line);
            return;
        }
        CDCore::dev::append_line(s_log_path, line);
    }

    /// Formats one line into a fixed stack buffer and forwards it to log_msg.
    template <class... Args> void logf(const char *fmt, Args... args) noexcept
    {
        char buffer[768];
        if (std::snprintf(buffer, sizeof(buffer), fmt, args...) > 0)
        {
            log_msg(buffer);
        }
    }

    // Process gate.

    /**
     * @brief Reports whether this process is the game the loader targets.
     * @details ASI hosts fan the loader out into every executable in the game directory, crash handlers and
     *          launcher stubs included. Only the game process gets a control thread.
     */
    [[nodiscard]] bool running_in_game_process() noexcept
    {
        char path[MAX_PATH]{};
        const DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
        {
            return false;
        }
        const char *const slash = std::strrchr(path, '\\');
        const char *const exe = (slash != nullptr) ? slash + 1 : path;
        return _stricmp(exe, PROCESS_NAME) == 0;
    }

    // Paths.

    /// Returns the directory of module @p self, with a trailing separator, or an empty string on failure.
    std::string loader_dir(HMODULE self)
    {
        char path[MAX_PATH]{};
        if (GetModuleFileNameA(self, path, MAX_PATH) == 0)
        {
            return {};
        }
        char *const slash = std::strrchr(path, '\\');
        if (slash == nullptr)
        {
            return {};
        }
        slash[1] = '\0';
        return std::string{path};
    }

    /**
     * @brief Formats the size and last-write time of @p path as "bytes=N built=YYYY-MM-DD HH:MM:SS".
     * @details This is the loader's own witness of what it mapped, taken from the file it copied. It cannot go stale
     *          the way a compiled-in __TIME__ can, because the loader reads it from disk at load time instead of
     *          baking it into one translation unit at compile time.
     */
    std::string file_identity(const std::string &path)
    {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info) == 0)
        {
            return "bytes=? built=?";
        }

        SYSTEMTIME utc{};
        SYSTEMTIME local{};
        char stamp[32] = "?";
        if (FileTimeToSystemTime(&info.ftLastWriteTime, &utc) != 0 &&
            SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) != 0)
        {
            (void)std::snprintf(
                stamp,
                sizeof(stamp),
                "%04u-%02u-%02u %02u:%02u:%02u",
                local.wYear,
                local.wMonth,
                local.wDay,
                local.wHour,
                local.wMinute,
                local.wSecond
            );
        }

        const auto bytes =
            (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | static_cast<std::uint64_t>(info.nFileSizeLow);
        char out[96];
        (void)std::snprintf(out, sizeof(out), "bytes=%llu built=%s", static_cast<unsigned long long>(bytes), stamp);
        return out;
    }

    /// Returns the full path of staged generation number @p generation.
    std::string generation_path(unsigned generation)
    {
        char name[160];
        std::snprintf(name, sizeof(name), "%s%04u%s", s_generation_prefix.c_str(), generation, GENERATION_SUFFIX);
        return s_loader_dir + name;
    }

    /// Moves one staged file into the loader directory, and does nothing when the file is absent.
    void move_staged_file(const std::string &staging_dir, const std::string &filename)
    {
        const std::string src = staging_dir + filename;
        if (GetFileAttributesA(src.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return;
        }
        CopyFileA(src.c_str(), (s_loader_dir + filename).c_str(), FALSE);
        DeleteFileA(src.c_str());
    }

    /// Promotes a freshly built logic DLL (and its PDB) out of the staging directory.
    void promote_from_staging()
    {
        const std::string staging_dir = s_loader_dir + STAGING_SUBDIR + "\\";
        const std::string staged_dll = staging_dir + s_logic_dll_name;
        if (GetFileAttributesA(staged_dll.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return; // nothing new was built
        }
        if (!CopyFileA(staged_dll.c_str(), (s_loader_dir + s_logic_dll_name).c_str(), FALSE))
        {
            log_msg("Failed to promote the staged logic DLL");
            return;
        }
        DeleteFileA(staged_dll.c_str());
        move_staged_file(staging_dir, s_logic_pdb_name);
        log_msg("Promoted staged logic DLL");
    }

    /**
     * @brief Deletes staged copies left by earlier runs.
     * @details A copy that still backs a mapped image stays locked, so failures are expected and ignored.
     */
    void sweep_stale_generations()
    {
        unsigned removed = 0;
        WIN32_FIND_DATAA found{};
        const std::string pattern = s_loader_dir + s_generation_prefix + "*" + GENERATION_SUFFIX;
        const HANDLE search = FindFirstFileA(pattern.c_str(), &found);
        if (search == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            if (DeleteFileA((s_loader_dir + found.cFileName).c_str()))
            {
                ++removed;
            }
        } while (FindNextFileA(search, &found));
        FindClose(search);
        if (removed != 0)
        {
            logf("Swept %u stale generation file(s) from previous runs", removed);
        }
    }

    // Release proofs.

    /**
     * @brief Waits for an address inside the retired image to stop belonging to any loaded module.
     * @details Address-based rather than name-based: it asks the loader the exact question that matters, and
     *          UNCHANGED_REFCOUNT keeps the probe from perturbing the count it measures. Probing a freed address is
     *          safe: the call fails, which IS the answer.
     */
    [[nodiscard]] bool wait_for_unmap(const void *address) noexcept
    {
        if (address == nullptr)
        {
            return false; // no probe address means no proof
        }
        for (DWORD waited = 0; waited < UNMAP_TIMEOUT_MS; waited += UNMAP_POLL_MS)
        {
            HMODULE owner = nullptr;
            if (GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(address),
                    &owner
                ) == 0)
            {
                return true;
            }
            Sleep(UNMAP_POLL_MS);
        }
        return false;
    }

    /**
     * @brief Confirms the retired generation left no lease open on the resident wheel host.
     * @details The host allows one lease at a time, so a successful open proves the generation closed its own. A
     *          failed close leaves host state unknown, which is as serious as a failed unmap.
     */
    [[nodiscard]] bool host_lease_is_closed(std::uint64_t generation_id) noexcept
    {
        WheelHostLease probe = 0;
        const std::int32_t open_status =
            s_wheel_host.open_lease(s_wheel_host.host_context, LEASE_PROBE_OWNER, generation_id, &probe);
        if (open_status != DMK_WHEELHOST_OK)
        {
            logf(
                "Generation %llu left its wheel-host lease open (status %d)",
                static_cast<unsigned long long>(generation_id),
                open_status
            );
            return false;
        }
        const std::int32_t close_status =
            s_wheel_host.close_lease(s_wheel_host.host_context, probe, LEASE_PROBE_OWNER, generation_id);
        if (close_status != DMK_WHEELHOST_OK)
        {
            s_restart_required = true;
            logf("The loader could not close its wheel-host probe lease (status %d); restart required", close_status);
            return false;
        }
        return true;
    }

    /// Retires a generation: lease probe, FreeLibrary once, then proof of unmap.
    [[nodiscard]] bool release_generation(Generation &generation) noexcept
    {
        if (generation.module == nullptr)
        {
            return true;
        }
        // Only meaningful when a generation actually leased the host.
        if (s_wheel_host_live && !host_lease_is_closed(generation.generation_id))
        {
            return false;
        }

        // Shutdown removed every hook, so no NEW detour entry can occur. What it cannot drain is a game thread
        // already inside a per-frame detour body in this image. Those return in microseconds.
        Sleep(POST_SHUTDOWN_MS);

        const HMODULE module = generation.module;
        const void *const address = generation.unmap_address;
        generation.module = nullptr;
        generation.init = nullptr;
        generation.shutdown = nullptr;
        generation.revision = nullptr;

        if (FreeLibrary(module) == 0)
        {
            logf("FreeLibrary failed (error %lu); restart required", GetLastError());
            s_restart_required = true;
            return false;
        }
        if (!wait_for_unmap(address))
        {
            return false;
        }
        DeleteFileA(generation.path.c_str());
        return true;
    }

    // Generation lifecycle.

    [[nodiscard]] CdReloadInitRequest make_request(std::uint64_t generation_id) noexcept
    {
        return CdReloadInitRequest{
            .struct_size = static_cast<std::uint32_t>(sizeof(CdReloadInitRequest)),
            .abi_version = CDCORE_RELOAD_ABI_VERSION,
            .generation_id = generation_id,
            .expected_host_identity = s_wheel_host_live ? s_host_identity : 0,
            .wheel_host = s_wheel_host_live ? &s_wheel_host : nullptr,
        };
    }

    [[nodiscard]] bool load_generation()
    {
        Generation generation;
        ++s_generation_counter;
        generation.generation_id = s_generation_counter;
        generation.path = generation_path(s_generation_counter);

        if (!CopyFileA((s_loader_dir + s_logic_dll_name).c_str(), generation.path.c_str(), FALSE))
        {
            logf("Staging generation %04u failed (error %lu)", s_generation_counter, GetLastError());
            return false;
        }

        generation.module = LoadLibraryA(generation.path.c_str());
        if (generation.module == nullptr)
        {
            logf("LoadLibrary failed (error %lu)", GetLastError());
            DeleteFileA(generation.path.c_str());
            return false;
        }
        generation.init = reinterpret_cast<InitFn>(
            reinterpret_cast<void *>(GetProcAddress(generation.module, CDCORE_RELOAD_INIT_SYMBOL))
        );
        generation.shutdown = reinterpret_cast<ShutdownFn>(
            reinterpret_cast<void *>(GetProcAddress(generation.module, CDCORE_RELOAD_SHUTDOWN_SYMBOL))
        );
        generation.revision = reinterpret_cast<RevisionFn>(
            reinterpret_cast<void *>(GetProcAddress(generation.module, CDCORE_RELOAD_REVISION_SYMBOL))
        );
        generation.unmap_address = reinterpret_cast<const void *>(generation.init);

        if (generation.init == nullptr || generation.shutdown == nullptr || generation.revision == nullptr)
        {
            log_msg("The logic DLL is missing Init/Shutdown/Revision exports");
            (void)release_generation(generation);
            return false;
        }

        const CdReloadInitRequest request = make_request(generation.generation_id);
        if (generation.init(&request) != CDCORE_RELOAD_OK)
        {
            log_msg("Init refused the load");
            (void)release_generation(generation);
            return false;
        }

        // Two identities, because they answer different questions and can disagree. file_identity is what this
        // loader mapped, read from the image on disk. revision is the logic DLL's self-reported source version, and
        // its embedded build stamp only advances when mod_logic.cpp itself recompiles - a build that changed any
        // other file relinks the DLL and leaves that stamp behind. Trust the file identity to tell new bytes from a
        // replay. Read revision as the version string it is.
        const char *const revision = generation.revision();
        logf(
            "Generation %04u is live - %s [%s]",
            s_generation_counter,
            revision != nullptr ? revision : "unknown",
            file_identity(generation.path).c_str()
        );
        s_current.emplace(std::move(generation));
        return true;
    }

    /**
     * @brief Retires the live generation.
     * @return false when it must stay mapped. The caller must NOT load another over it.
     */
    [[nodiscard]] bool unload_current() noexcept
    {
        if (!s_current.has_value())
        {
            return true;
        }
        if (s_current->shutdown() == 0)
        {
            // Shutdown already ran its teardown before refusing, so the mod is inert but the image must stay mapped.
            // Re-entering Init on this handle restores it in place - the best available outcome, and it is still the
            // OLD code.
            log_msg("Shutdown refused the unload; the generation stays mapped");
            if (s_current->init != nullptr)
            {
                const CdReloadInitRequest request = make_request(s_current->generation_id);
                if (s_current->init(&request) == CDCORE_RELOAD_OK)
                {
                    log_msg("Re-initialized the existing generation in place; still running the OLD code");
                }
            }
            return false;
        }

        Generation retiring = std::move(*s_current);
        s_current.reset();
        if (!release_generation(retiring))
        {
            // The image stays mapped for the process. Its code can still be reachable, so the reload budget shrinks
            // and the loader eventually stops rather than stacking unknown state.
            ++s_retained_generations;
            logf(
                "Generation %llu could not be proved gone; %u of %u retained images used",
                static_cast<unsigned long long>(retiring.generation_id),
                s_retained_generations,
                MAX_RETAINED_GENERATIONS
            );
            if (s_retained_generations >= MAX_RETAINED_GENERATIONS)
            {
                s_restart_required = true;
                log_msg("The retained-generation budget is exhausted; restart the game to reload again");
            }
        }
        return true;
    }

    /**
     * @brief Retires the live generation, promotes the staged build, and loads it.
     * @details The order is fixed: the outgoing image must be proved gone before another maps over it. A refused
     *          unload leaves the current generation live and returns without a load.
     */
    void reload_once()
    {
        if (s_restart_required)
        {
            log_msg("A previous reload could not be proved safe; restart the game");
            return;
        }
        log_msg("Numpad 0 released - reloading logic DLL...");
        if (!unload_current())
        {
            return; // refused: the current generation stays live
        }
        promote_from_staging();
        if (!load_generation())
        {
            log_msg("Reload FAILED - no generation is live");
        }
    }

    /**
     * @brief Control thread: sets the loader paths up, starts the wheel host, then polls the reload key.
     * @details Runs the one-time setup in order (paths, logs, stale sweep, wheel host, first generation) before the
     *          poll loop, so no generation maps before the resident host exists. On exit it calls the live
     *          generation's Shutdown and never calls FreeLibrary.
     * @param param The loader module handle, which names the directory every path derives from.
     * @return Always 0. The thread exits when DLL_PROCESS_DETACH clears the run flag.
     */
    DWORD WINAPI loader_thread(LPVOID param)
    {
        s_loader_dir = loader_dir(static_cast<HMODULE>(param));
        s_log_prefix = std::string{"["} + MOD_NAME + " Loader] ";
        s_logic_dll_name = std::string{MOD_NAME} + GENERATION_SUFFIX;
        s_logic_pdb_name = std::string{MOD_NAME} + ".logic.pdb";
        s_generation_prefix = std::string{MOD_NAME} + ".gen";

        const std::string loader_log = s_loader_dir + MOD_NAME + CDCore::dev::LOADER_LOG_SUFFIX;
        std::snprintf(s_log_path, sizeof(s_log_path), "%s", loader_log.c_str());
        (void)DeleteFileA(s_log_path); // one log per game run, holding every generation

        // Truncate the MOD log here too, exactly once, before any generation opens it. Each generation starts its
        // Session with LogOpenMode::Append, because the default Truncate makes a reload erase the outgoing
        // generation's teardown records - including the retention warnings that explain a retained image. Append
        // cannot tell "next generation" from "next game run", so owning the reset here gives one file per game run,
        // holding every generation within that run.
        {
            const std::string mod_log = s_loader_dir + MOD_NAME + ".log";
            (void)DeleteFileA(mod_log.c_str());
        }

        log_msg("Loader thread started");
        sweep_stale_generations();

        // The resident wheel host is started once, before the first generation, and never stopped. It owns the
        // permanent wheel keepalive so no logic generation has to. A host that refuses to start is not fatal: the
        // generation then uses the local message-hook backend and is charged as a retained image if the user binds a
        // wheel combo.
        const std::int32_t host_status = wheel_host_start(
            0,
            DMK_WHEELHOST_ABI_VERSION,
            static_cast<std::uint32_t>(sizeof(s_wheel_host)),
            &s_wheel_host
        );
        if (host_status == DMK_WHEELHOST_OK)
        {
            s_host_identity = s_wheel_host.host_identity;
            s_wheel_host_live = true;
            log_msg("Resident wheel host started (owns the wheel keepalive for the process)");
        }
        else
        {
            logf(
                "The resident wheel host failed to start (status %d); wheel bindings pin each generation",
                host_status
            );
        }

        promote_from_staging();
        if (!load_generation())
        {
            log_msg("Initial logic DLL load failed - press Numpad 0 after rebuilding to retry");
        }

        bool was_key_down = false;
        while (s_running.load(std::memory_order_relaxed))
        {
            Sleep(CONTROL_POLL_MS);
            const bool is_key_down = (GetAsyncKeyState(RELOAD_VK) & 0x8000) != 0;
            if (was_key_down && !is_key_down) // reload on the key-up edge so a held key cannot retrigger
            {
                if (!s_reloading.exchange(true, std::memory_order_acq_rel))
                {
                    reload_once();
                    s_reloading.store(false, std::memory_order_release);
                }
            }
            was_key_down = is_key_down;
        }

        // Terminal path: the thread exits because DLL_PROCESS_DETACH cleared s_running. Only run the logic's
        // Shutdown here. Do NOT FreeLibrary. DllMain joins this thread under the OS loader lock, which FreeLibrary
        // also needs, so that path deadlocks until the join times out.
        if (s_current.has_value() && s_current->shutdown != nullptr)
        {
            (void)s_current->shutdown();
        }
        return 0;
    }
} // namespace

/**
 * @brief Loader entry point: starts the control thread in the game process and joins it on detach.
 * @param module This module's handle, forwarded to the control thread as its path root.
 * @param reason The loader notification.
 * @return Always TRUE. A sibling executable stays inert instead of failing its load.
 */
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        if (!running_in_game_process())
        {
            return TRUE; // loaded into a sibling executable; stay inert
        }
        s_running.store(true, std::memory_order_relaxed);
        s_thread = CreateThread(nullptr, 0, loader_thread, module, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        s_running.store(false, std::memory_order_relaxed);
        if (s_thread != nullptr)
        {
            WaitForSingleObject(s_thread, 2000);
            CloseHandle(s_thread);
            s_thread = nullptr;
        }
    }
    return TRUE;
}
