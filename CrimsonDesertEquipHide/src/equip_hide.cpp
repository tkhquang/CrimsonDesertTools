#include "equip_hide.hpp"
#include "categories.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace EquipHide
{
    // =========================================================================
    // Hook target: sub_140818340 — visibility decision function
    //
    // Hook point: movzx eax, byte ptr [r13+1Ch]; cmp al, 3
    //
    // Register layout at hook point:
    //   R10 = pointer to part hash DWORD (IndexedStringA ID)
    //   R13 = pointer to PartInOutSocket struct (Visible byte at +0x1C)
    //   R8B = exclusion-list flag
    //
    // To hide a part: set [R13+0x1C] = 2 (Out-only visibility)
    //
    // Cascading AOB patterns (tried in order until one matches):
    //   P1: Direct hook-site pattern (most precise, least resilient)
    //   P2: Wider context pattern encoding array iteration structure
    //       (survives register reallocation, needs +0x36 offset)
    //   P3: Short core pattern (last resort, 7 bytes)
    //
    // See .idea/research/update_resilience.md for full analysis.
    // =========================================================================
    struct AobCandidate
    {
        const char *name;
        const char *pattern;
        ptrdiff_t offsetToHook; // bytes from AOB match to the actual hook point
    };

    static constexpr AobCandidate k_aobCandidates[] = {
        {"P1_DirectSite",
         "41 0F B6 45 1C 3C 03 74 ?? 45 84 C0 75 ?? 84 C0",
         0},

        {"P2_WiderContext",
         "45 32 C0 48 8B 4D ?? 48 8B 41 38 8B 49 40 48 C1 E1 04 48 03 C8 48 3B C1 74 ?? 41 8B 12",
         0x36},

        {"P3_ShortCore",
         "41 0F B6 45 1C 3C 03",
         0},
    };

    // =========================================================================
    // Mid-hook callback (SEH-protected)
    //
    // Read part hash from [R10], classify by range, and force Visible=2
    // for hidden categories.  Wrapped in SEH to prevent game crash if the
    // mod is outdated and register layout has changed.
    // =========================================================================
    /// Core logic — no SEH here so C++ objects (SafetyHookContext&) are fine.
    static void on_vis_check_impl(SafetyHookContext &ctx)
    {
        auto r10 = ctx.r10;
        if (r10 < 0x10000)
            return;

        auto partHash = *reinterpret_cast<const uint32_t *>(r10);

        // Quick range check before map lookup — skip obviously out-of-range hashes
        if (partHash < 0x0F00 || (partHash > 0x0F50 && partHash < 0xAD00) || partHash > 0xBFFF)
            return;

        const auto cat = classify_part(partHash);
        if (!cat)
            return;

        auto r13 = ctx.r13;
        if (r13 < 0x10000)
            return;

        if (is_category_hidden(*cat))
        {
            auto *visPtr = reinterpret_cast<uint8_t *>(r13 + 0x1C);
            *visPtr = 2; // Force Out-only visibility
        }
    }

    /// SEH wrapper — catches access violations if mod is outdated and
    /// register layout has changed.  Separate function because MSVC SEH
    /// cannot coexist with C++ destructors in the same frame.
#ifdef _MSC_VER
    static int seh_filter(unsigned int /*code*/) { return EXCEPTION_EXECUTE_HANDLER; }

    static void on_vis_check(SafetyHookContext &ctx)
    {
        __try
        {
            on_vis_check_impl(ctx);
        }
        __except (seh_filter(GetExceptionCode()))
        {
            // Silently swallow — mod is likely outdated, don't crash the game
        }
    }
#else
    // MinGW/GCC does not support MSVC-style SEH (__try/__except).
    // Fall back to an unprotected call — the pointer checks in
    // on_vis_check_impl already guard against the most common faults.
    static void on_vis_check(SafetyHookContext &ctx)
    {
        on_vis_check_impl(ctx);
    }
#endif

    // =========================================================================
    // AOB scan with unpack retry
    //
    // Packed/protected binaries may decompress code into dynamically
    // allocated memory outside the main module.  Scan all readable-
    // executable regions in the process to find the hook target
    // regardless of where the unpacker places the code.
    // =========================================================================

    struct CompiledCandidate
    {
        const AobCandidate *source;
        DMK::Scanner::CompiledPattern compiled;
    };

    /// Scan all executable committed memory regions for the AOB patterns.
    /// Returns the resolved hook address and sets matchedSource on success.
    static uintptr_t scan_for_hook_target(
        const std::vector<CompiledCandidate> &candidates,
        const AobCandidate *&matchedSource)
    {
        const uint8_t *addr = nullptr;
        MEMORY_BASIC_INFORMATION mbi;

        while (VirtualQuery(addr, &mbi, sizeof(mbi)))
        {
            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY)))
            {
                const auto regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);

                for (const auto &c : candidates)
                {
                    const auto *match = DMK::Scanner::find_pattern(
                        reinterpret_cast<const std::byte *>(regionBase),
                        mbi.RegionSize, c.compiled);
                    if (match)
                    {
                        matchedSource = c.source;
                        return reinterpret_cast<uintptr_t>(match) + c.source->offsetToHook;
                    }
                }
            }

            addr = static_cast<const uint8_t *>(mbi.BaseAddress) + mbi.RegionSize;
        }

        return 0;
    }

    // =========================================================================
    // Config
    // =========================================================================
    static void load_config()
    {
        DMK::Config::register_string("General", "LogLevel", "Log Level", [](const std::string &val)
                                     {
                auto& logger = DMK::Logger::get_instance();
                logger.set_log_level(DMK::Logger::string_to_log_level(val)); }, "Info");

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            const auto cat = static_cast<Category>(i);
            const std::string section{category_section(cat)};

            DMK::Config::register_bool(section, "Enabled", section + " Enabled", [i](bool val)
                                       { category_states()[i].enabled.store(val, std::memory_order_relaxed); }, true);

            DMK::Config::register_key_combo(section, "Hotkey", section + " Hotkey", [i](const DMK::Config::KeyComboList &combos)
                                            { category_states()[i].hotkeyCombos = combos; }, "V");

            DMK::Config::register_bool(section, "DefaultHidden", section + " Default Hidden", [i](bool val)
                                       { category_states()[i].hidden.store(val, std::memory_order_relaxed); }, false);

            DMK::Config::register_string(section, "Parts", section + " Parts", [cat](const std::string &val)
                                         { register_parts(cat, val); }, std::string{default_parts(cat)});
        }

        DMK::Config::load(INI_FILE);
        DMK::Config::log_all();
        build_part_lookup();
    }

    // =========================================================================
    // Hotkey registration — categories sharing the same key toggle together
    // =========================================================================
    static void register_hotkeys()
    {
        auto &inputMgr = DMK::InputManager::get_instance();
        auto &states = category_states();

        struct HotkeyGroup
        {
            DMK::Config::KeyComboList combos;
            std::vector<std::size_t> categoryIndices;
        };

        std::unordered_map<std::string, HotkeyGroup> groups;

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            if (!states[i].enabled.load(std::memory_order_relaxed))
                continue;
            if (states[i].hotkeyCombos.empty())
                continue;

            std::string key;
            for (const auto &combo : states[i].hotkeyCombos)
            {
                for (const auto &mod : combo.modifiers)
                    key += std::to_string(static_cast<int>(mod.source)) + ":" + std::to_string(mod.code) + "+";
                for (const auto &k : combo.keys)
                    key += std::to_string(static_cast<int>(k.source)) + ":" + std::to_string(k.code) + ",";
                key += "|";
            }

            auto &group = groups[key];
            if (group.combos.empty())
                group.combos = states[i].hotkeyCombos;
            group.categoryIndices.push_back(i);
        }

        auto &logger = DMK::Logger::get_instance();

        for (auto &[key, group] : groups)
        {
            std::string bindingName = "ToggleEquip";
            for (auto idx : group.categoryIndices)
                bindingName += std::string("_") + std::string(category_section(static_cast<Category>(idx)));

            auto indices = group.categoryIndices;

            inputMgr.register_press(
                bindingName,
                group.combos,
                [indices, &logger]()
                {
                    auto &st = category_states();
                    const bool newHidden = !st[indices[0]].hidden.load(std::memory_order_relaxed);
                    for (auto idx : indices)
                        st[idx].hidden.store(newHidden, std::memory_order_relaxed);

                    std::string catNames;
                    for (auto idx : indices)
                    {
                        if (!catNames.empty())
                            catNames += ", ";
                        catNames += category_section(static_cast<Category>(idx));
                    }
                    logger.info("Equip hide toggled [{}]: {}", catNames, newHidden ? "HIDDEN" : "VISIBLE");
                });

            logger.info("Registered hotkey binding '{}' for {} categories",
                        bindingName, indices.size());
        }
    }

    // =========================================================================
    // Public interface
    // =========================================================================
    bool init()
    {
        auto &logger = DMK::Logger::get_instance();

        logger.info("=== {} v{} ===", MOD_NAME, MOD_VERSION);

        load_config();

        // Pre-compile AOB patterns once
        std::vector<CompiledCandidate> compiledCandidates;
        for (const auto &candidate : k_aobCandidates)
        {
            auto compiled = DMK::Scanner::parse_aob(candidate.pattern);
            if (compiled)
                compiledCandidates.push_back({&candidate, std::move(*compiled)});
            else
                logger.warning("Failed to parse AOB pattern '{}'", candidate.name);
        }

        if (compiledCandidates.empty())
        {
            logger.error("No valid AOB patterns available.");
            return false;
        }

        // Scan all executable memory regions for the hook target.
        // The process gate in dllmain ensures we run after the protector
        // has finished unpacking, so a single scan pass is sufficient.
        const AobCandidate *matchedSource = nullptr;
        uintptr_t hookAddr = scan_for_hook_target(compiledCandidates, matchedSource);

        if (hookAddr == 0)
        {
            logger.error("No AOB pattern matched. The mod may be outdated for this game version.");
            return false;
        }

        auto &hookMgr = DMK::HookManager::get_instance();
        auto hookResult = hookMgr.create_mid_hook("EquipVisCheck", hookAddr, on_vis_check);

        if (!hookResult.has_value())
        {
            logger.error("Hook creation failed at 0x{:X}: {}",
                         hookAddr, DetourModKit::Hook::error_to_string(hookResult.error()));
            return false;
        }

        logger.info("Hook installed via pattern '{}' at 0x{:X}",
                    matchedSource->name, hookAddr);

        register_hotkeys();

        auto &inputMgr = DMK::InputManager::get_instance();
        inputMgr.start();

        logger.info("Equip hide system initialized");
        return true;
    }

    void shutdown()
    {
        DMK::Logger::get_instance().info("{} shutting down...", MOD_NAME);
        DMK_Shutdown();
    }

} // namespace EquipHide
