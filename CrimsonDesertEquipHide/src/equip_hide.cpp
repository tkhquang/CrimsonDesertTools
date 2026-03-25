#include "equip_hide.hpp"
#include "categories.hpp"
#include "version.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace EquipHide
{
    // =========================================================================
    // Constants
    // =========================================================================
    static constexpr const char *MOD_VERSION = VERSION_STRING;
    static uint32_t s_initDelayMs = 3000;

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
    // Config
    // =========================================================================
    static void load_config()
    {
        DMK::Config::register_int("General", "InitDelayMs", "Init Delay (ms)", [](int val)
                                  { s_initDelayMs = static_cast<uint32_t>(val); }, 3000);

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

        DMK::Config::load("CrimsonDesertEquipHide.ini");
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

        logger.info("=== CrimsonDesertEquipHide v{} ===", MOD_VERSION);

        load_config();

        // Wait for unpack: configurable delay + SizeOfImage poll
        if (s_initDelayMs > 0)
        {
            logger.info("Waiting {}ms initial delay...", s_initDelayMs);
            Sleep(s_initDelayMs);
        }

        logger.info("Polling for game unpack (SizeOfImage > 10 MB, timeout 5 min)...");
        constexpr int kMaxPolls = 3000;
        bool unpacked = false;
        for (int poll = 0; poll < kMaxPolls; ++poll)
        {
            const auto *dh = reinterpret_cast<const IMAGE_DOS_HEADER *>(GetModuleHandleW(nullptr));
            const auto *nh = reinterpret_cast<const IMAGE_NT_HEADERS *>(
                reinterpret_cast<const uint8_t *>(dh) + dh->e_lfanew);
            if (nh->OptionalHeader.SizeOfImage > 10u * 1024u * 1024u)
            {
                unpacked = true;
                break;
            }
            Sleep(100);
        }

        if (!unpacked)
            logger.error("Timed out waiting for game to unpack.");
        else
            logger.info("Game unpacked.");

        HMODULE gameModule = GetModuleHandleW(nullptr);
        const auto moduleBase = reinterpret_cast<uintptr_t>(gameModule);
        const auto *dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER *>(gameModule);
        const auto *ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS *>(
            reinterpret_cast<const uint8_t *>(gameModule) + dosHeader->e_lfanew);
        auto moduleSize = static_cast<size_t>(ntHeaders->OptionalHeader.SizeOfImage);

        {
            const auto *firstSection = IMAGE_FIRST_SECTION(ntHeaders);
            size_t maxExtent = 0;
            for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i)
            {
                auto secEnd = static_cast<size_t>(firstSection[i].VirtualAddress) + firstSection[i].Misc.VirtualSize;
                if (secEnd > maxExtent)
                    maxExtent = secEnd;
            }
            if (maxExtent > moduleSize)
                moduleSize = maxExtent;
        }

        logger.info("Game module: base=0x{:X}, size=0x{:X} ({:.1f} MB)",
                    moduleBase, moduleSize, moduleSize / (1024.0 * 1024.0));

        // Install mid-hook using cascading AOB scan (try each pattern until one works)
        auto &hookMgr = DMK::HookManager::get_instance();
        bool hookInstalled = false;

        for (const auto &candidate : k_aobCandidates)
        {
            logger.info("Trying AOB pattern '{}' (offset +0x{:X})...",
                        candidate.name, candidate.offsetToHook);

            auto hookResult = hookMgr.create_mid_hook_aob(
                "EquipVisCheck",
                moduleBase,
                moduleSize,
                candidate.pattern,
                candidate.offsetToHook,
                on_vis_check);

            if (hookResult.has_value())
            {
                logger.info("Hook '{}' installed via pattern '{}' successfully",
                            hookResult.value(), candidate.name);
                hookInstalled = true;
                break;
            }

            logger.warning("Pattern '{}' failed: {}",
                           candidate.name,
                           DetourModKit::Hook::error_to_string(hookResult.error()));
        }

        if (!hookInstalled)
        {
            logger.error("All AOB patterns failed. The mod may be outdated for this game version.");
            logger.error("The game will continue normally without equipment hiding.");
            // Don't return false — still register hotkeys so the mod doesn't crash,
            // and the user sees toggle messages (even though they have no effect).
        }

        register_hotkeys();

        auto &inputMgr = DMK::InputManager::get_instance();
        inputMgr.start();

        logger.info("Equip hide system initialized");
        return true;
    }

    void shutdown()
    {
        DMK::Logger::get_instance().info("CrimsonDesertEquipHide shutting down...");
        DMK_Shutdown();
    }

} // namespace EquipHide
