#pragma once

#include <DetourModKit.hpp>

namespace EquipHide
{
    /**
     * @brief Register every hotkey binding and INI key with DetourModKit.
     *
     * Each binding is wired via DMK::Config::register_press_combo, which
     * fuses INI registration, default-combo parsing, and the
     * InputManager press registration into a single call. The returned
     * InputBindingGuards are stashed in a process-lifetime static vector
     * so the cancellation flag survives until InputManager teardown.
     *
     * Must be invoked before DMK::Config::load() (so the registered
     * setters fire on the first load pass) and before
     * InputManager::start() (so the bindings are picked up by the
     * poller).
     */
    void register_hotkeys();

    /** @brief Flush visibility state: re-resolve players, re-inject, re-write vis bytes. */
    void flush_visibility() noexcept;

    /**
     * @brief Drops the guard vector populated by register_hotkeys().
     *
     * Releases the per-binding cancellation flags so a subsequent
     * register_hotkeys() pass starts from an empty stash. The
     * InputManager poller and its registered bindings are torn down
     * by DMK_Shutdown() immediately before this helper runs, so this
     * call only resets the local guard vector.
     */
    void clear_hotkey_guards() noexcept;

} // namespace EquipHide
