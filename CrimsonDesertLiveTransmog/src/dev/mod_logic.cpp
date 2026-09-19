/**
 * @file mod_logic.cpp
 * @brief The hot-reloaded half of the dev build: one generation of mod logic behind a C ABI.
 *
 * @details The resident loader owns the process; this DLL owns one generation. The loader calls Init() after
 *          LoadLibrary and Shutdown() before FreeLibrary. There is no DllMain bootstrap on this path, so Init() owns
 *          the Session directly and Shutdown() drops it.
 *
 *          Shutdown()'s return value is an UNMAP AUTHORIZATION, not a status. Returning success while a detour body
 *          in this image is still reachable is what turns a reload into a stale image and then a crash. The verdict
 *          follows the DetourModKit hot-reload guide: drain, clear hooks newest-first, drop the Session, then
 *          read module pins AS STATE.
 *
 *          "As state" is the part that matters. A DELTA of diagnostics::total_intentional_leaks() across teardown
 *          reads zero for a wheel keepalive, because that pin is booked at install time, so it would authorize
 *          unmapping an image that can never unmap. diagnostics::module_pin_count() reports the open reference itself
 *          and stays readable after ~Session.
 *
 *          Only compiled when TRANSMOG_DEV_BUILD is set by the dev preset. Includes use explicit `../` relative paths
 *          so IntelliSense resolves them even when parsing the file standalone.
 */

#include "../constants.hpp"
#include "../overlay.hpp"
#include "../transmog.hpp"
#include "../version.hpp"

#include "loader_log.hpp"
#include "protocol.h"

#include <DetourModKit/async_logger_config.hpp>
#include <DetourModKit/diagnostics.hpp>
#include <DetourModKit/filesystem.hpp>
#include <DetourModKit/logger.hpp>
#include <DetourModKit/session.hpp>

#include <Windows.h>

#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace
{
    /// The live Session for this generation. Empty between Shutdown() and the next Init().
    std::optional<DMK::Session> s_session;

    /// Set when Transmog::shutdown() could not prove every hooked prologue was restored.
    bool s_hook_restore_failed = false;

    /**
     * @brief Appends one line to the LOADER's log, which outlives every generation.
     * @details The unload verdict is computed after ~Session, so DMK::log() is gone by then. The line takes the
     *          loader log's own format (CrimsonDesertCore/dev/loader_log.hpp), so it sorts in with the loader's
     *          records. A line sent only to OutputDebugStringA hides from anyone without a debugger attached,
     *          which is exactly how a refusal loop goes unexplained.
     * @note get_runtime_directory() allocates, and catch(...) handlers call this with an exception already in
     *       flight, so the path build runs inside try/catch to hold the noexcept boundary.
     */
    void append_loader_log(const char *line) noexcept
    {
        std::wstring log_path;
        try
        {
            log_path = CDCore::dev::loader_log_path(DMK::filesystem::get_runtime_directory(), Transmog::MOD_NAME);
        }
        catch (...)
        {
            CDCore::dev::echo_to_debugger(line);
            return;
        }
        CDCore::dev::append_line(log_path.c_str(), line);
    }
} // namespace

extern "C"
{
    /**
     * @brief Reports this generation's source version.
     * @details Exported so the LOADER can log it even when Init() fails, which is exactly when knowing the version
     *          matters most.
     * @warning This is NOT a witness of which bytes were mapped. The embedded build stamp comes from __DATE__ /
     *          __TIME__ in THIS translation unit, so it only advances when this file recompiles; a build that
     *          changed any other source relinks the DLL and leaves the stamp behind. The loader logs the mapped
     *          file's own size and write time alongside this string, and that pair is the authoritative identity.
     */
    __declspec(dllexport) const char *Revision() noexcept
    {
        return TRANSMOG_VERSION_TAG " (" __DATE__ " " __TIME__ ")";
    }

    /**
     * @brief Starts one generation.
     * @param request Loader-owned request. Its wheel-host table is valid for the whole process.
     * @return CDCORE_RELOAD_OK when the generation is live, otherwise zero after rollback.
     */
    __declspec(dllexport) unsigned Init(const CdReloadInitRequest *request) noexcept
    {
        // Validate the whole request before touching a field: a stale generation left in the deploy directory must
        // fail loudly rather than read through a shifted layout.
        //
        // A NULL wheel host is a deliberate, supported choice, not an error. The loader decides whether a generation
        // leases its resident host; when it does not, this generation uses the local message-hook backend and books
        // its own permanent wheel keepalive, so its image is retained after teardown. The loader charges that against
        // its reload budget. A NON-null table must still match the expected identity, so a foreign table is never
        // accepted.
        if (request == nullptr || request->struct_size < sizeof(CdReloadInitRequest) ||
            request->abi_version != CDCORE_RELOAD_ABI_VERSION || request->generation_id == 0 || s_session.has_value())
        {
            append_loader_log("[LiveTransmog][DEV] Init rejected an invalid or stale reload request");
            return 0;
        }
        if (request->wheel_host != nullptr && (request->expected_host_identity == 0 ||
                                               request->wheel_host->host_identity != request->expected_host_identity))
        {
            append_loader_log("[LiveTransmog][DEV] Init rejected a foreign wheel-host table");
            return 0;
        }

        // The loader calls this through a C function pointer, so an exception must never unwind across the boundary.
        // Guard the whole body and return zero on any failure.
        try
        {
            DMK::AsyncLoggerConfig async_cfg;
            async_cfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;

            // LogOpenMode::Append is what keeps a reload diagnosable. Under the default Truncate, this generation's
            // first sink open erases the PREVIOUS generation's teardown records, which are the only lines that
            // explain a retained image. The loader truncates the file once per game run.
            auto opened = DMK::Session::start(
                DMK::ModInfo{
                    .name = Transmog::MOD_NAME,
                    .log_file = Transmog::LOG_FILE,
                    .game_process_name = Transmog::GAME_PROCESS_NAME,
                    .instance_mutex_prefix = Transmog::INSTANCE_MUTEX_PREFIX,
                    .log = async_cfg,
                    .log_open_mode = DMK::LogOpenMode::Append,
                    .log_source_stamp_mode = DMK::LogSourceStampMode::at_or_below(DMK::LogLevel::Trace),
                }
            );
            if (!opened)
            {
                append_loader_log("[LiveTransmog][DEV] Session::start failed");
                return 0;
            }
            s_session.emplace(std::move(*opened));
            s_hook_restore_failed = false;

            DMK::log().info("[DEV] Init generation {} - {}", request->generation_id, Revision());
            Transmog::version::log_version_info();

            if (auto ready = Transmog::init(*s_session, request->wheel_host); !ready)
            {
                DMK::log().error("[DEV] Transmog initialization FAILED ({})", ready.error().message());
                s_session.reset();
                return 0;
            }

            // The overlay's window procedure and its ReShade addon registration both live in THIS generation's
            // pages, so Shutdown() must retire them before the image can be unmapped. Best-effort: the mod still
            // works through hotkeys when the overlay cannot come up.
            (void)Transmog::init_overlay();
            return CDCORE_RELOAD_OK;
        }
        catch (...)
        {
            // The logger may not have come up yet, so report through the loader's file.
            append_loader_log("[LiveTransmog][DEV] Init threw an exception");
            s_session.reset();
            return 0;
        }
    }

    /**
     * @brief Tears this generation down and reports whether the image may be unmapped.
     * @return CDCORE_RELOAD_OK when the drain succeeded, every prologue was restored, and the only remaining module
     *         pins are the documented-inert ones.
     * @note The loader must keep the DLL mapped on a zero return.
     */
    __declspec(dllexport) unsigned Shutdown() noexcept
    {
        namespace diag = DMK::diagnostics;

        if (!s_session.has_value())
        {
            return 0;
        }

        try
        {
            DMK::log().info("[DEV] Shutdown called");

            // The overlay comes down first: its window class and window procedure both live in this image, so a
            // surviving window would leave the loader dispatching into pages it is about to unmap.
            Transmog::shutdown_overlay();

            // Mod teardown next, while this module's code pages are still mapped. It joins the workers, removes every
            // hook, and reports whether each prologue was restored.
            s_hook_restore_failed = !Transmog::shutdown();

            // The library's own authorization to unmap. It retires every binding and config setter DMK still owns and
            // delivers a held hold-combo's balancing edge while this module's code is mapped, so no callback body can
            // be entered afterwards.
            const DMK::LogicDllUnloadStatus drain = DMK::prepare_logic_dll_unload_all();
            if (drain != DMK::LogicDllUnloadStatus::SafeToUnload)
            {
                DMK::log().error(
                    "[DEV] drain refused unload (status {}); module stays mapped",
                    static_cast<int>(drain)
                );
                return 0;
            }

            // Drop the Session last: it shuts the library subsystems down in order, input included. The XInput layer
            // can decide to retain there, which is why the verdict below runs after this.
            s_session.reset();

            // Which pins are TOLERABLE is the part worth getting right. A retained XInput set and a wheel keepalive
            // are INERT after teardown, while every other nonzero reason can still identify live code. So the image
            // may stay mapped, but nothing in it can run, and the loader charges it against its reload budget.
            const std::size_t wheel_pins = diag::module_pin_count(diag::ModulePinReason::MessageHookKeepalive);
            const std::size_t xinput_self = diag::module_pin_count(diag::ModulePinReason::XInputKeepalive);
            const std::size_t xinput_targets = diag::module_pin_count(diag::ModulePinReason::XInputTarget);
            const std::size_t total_pins = diag::total_module_pins();
            const std::size_t inert_pins = wheel_pins + xinput_self + xinput_targets;

            char verdict[400];
            (void)std::snprintf(
                verdict,
                sizeof(verdict),
                "[LiveTransmog][DEV] unload verdict: wheel=%zu xinput_self=%zu xinput_targets=%zu total=%zu "
                "(inert=%zu), hooks_restored=%s",
                wheel_pins,
                xinput_self,
                xinput_targets,
                total_pins,
                inert_pins,
                s_hook_restore_failed ? "NO" : "yes"
            );
            append_loader_log(verdict);

            if (s_hook_restore_failed)
            {
                append_loader_log("[LiveTransmog][DEV] unload REFUSED: a hooked prologue was not restored");
                return 0;
            }
            if (total_pins != inert_pins)
            {
                append_loader_log(
                    "[LiveTransmog][DEV] unload REFUSED: a pin reason outside the documented-inert set "
                    "remains; live code may still be reachable"
                );
                return 0;
            }
            return CDCORE_RELOAD_OK;
        }
        catch (...)
        {
            append_loader_log("[LiveTransmog][DEV] Shutdown threw an exception; refusing unload");
            return 0;
        }
    }
} // extern "C"

/// The loader drives Init and Shutdown explicitly, so attach and detach have no work of their own.
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
