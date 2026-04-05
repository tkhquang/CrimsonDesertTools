#include "bald_fix.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

namespace EquipHide
{
    static PostfixEvalFn s_originalPostfixEval = nullptr;
    static PostfixCtxCreateFn s_originalCtxCreate = nullptr;

    /* NPC guard flag.  The PostfixEval context is a shared object reused
       for every actor — it contains the player's equipment but no actor
       identity.  The context-create wrapper (sub_1425F0580) positively
       identifies NPC evaluations and sets this flag.
       Default is false (not NPC) so PostfixEval suppresses hair-hiding
       unless we know it's an NPC — safe fallback for code paths that
       don't go through the context-create wrapper (e.g. equip events). */
    static thread_local bool t_isNpcEval = false;
    static DWORD s_mainThreadId = 0;

    /* Player descriptor cache.  Main-thread evaluations are always player;
       we cache their *a2 descriptor so we can recognize the same player
       on worker threads (e.g. equip events fire asynchronously). */
    static constexpr int k_maxCachedDescs = 4;
    static std::atomic<uintptr_t> s_playerDescs[k_maxCachedDescs]{};
    static std::atomic<int> s_playerDescCount{0};

    void set_postfix_eval_trampoline(PostfixEvalFn original)
    {
        s_originalPostfixEval = original;
    }

    void set_postfix_ctx_create_trampoline(PostfixCtxCreateFn original)
    {
        s_originalCtxCreate = original;
    }

    void set_baldfix_main_thread_id(DWORD id)
    {
        s_mainThreadId = id;
    }

    // ── Context-create hook ─────────────────────────────────────────────

    /* sub_1425F0580 is called once per actor during ConditionalPartPrefab
       evaluation.  Player evaluations always run on the main game thread;
       NPC evaluations run on worker threads.  The a2 descriptor is NOT an
       actor object — its pointer chain doesn't reach the player type byte.
       Thread identity is the only reliable discriminator available here. */

    static bool is_cached_player_desc(uintptr_t desc) noexcept
    {
        const int n = s_playerDescCount.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            if (s_playerDescs[i].load(std::memory_order_relaxed) == desc)
                return true;
        }
        return false;
    }

    static void cache_player_desc(uintptr_t desc) noexcept
    {
        if (is_cached_player_desc(desc))
            return;
        int n = s_playerDescCount.load(std::memory_order_relaxed);
        if (n < k_maxCachedDescs)
        {
            s_playerDescs[n].store(desc, std::memory_order_relaxed);
            s_playerDescCount.store(n + 1, std::memory_order_relaxed);
        }
    }

    __int64 __fastcall on_postfix_ctx_create(__int64 a1, __int64 *a2,
                                             char a3, __int64 *a4,
                                             __int64 *a5)
    {
        bool wasNpc = t_isNpcEval;

        uintptr_t desc = 0;
        __try { desc = *reinterpret_cast<const uintptr_t *>(a2); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (GetCurrentThreadId() == s_mainThreadId)
        {
            /* Main thread is always the player.  Cache the descriptor
               so we recognize it on worker threads (equip events). */
            t_isNpcEval = false;
            if (desc)
                cache_player_desc(desc);

            static std::atomic<bool> s_loggedPlayer{false};
            if (!s_loggedPlayer.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().trace(
                    "BaldFix: player eval on main thread (desc 0x{:X})", desc);
        }
        else if (desc && is_cached_player_desc(desc))
        {
            /* Worker thread but descriptor matches a cached player. */
            t_isNpcEval = false;

            static std::atomic<bool> s_loggedWorkerPlayer{false};
            if (!s_loggedWorkerPlayer.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().trace(
                    "BaldFix: player eval on worker thread (cached desc 0x{:X})", desc);
        }
        else
        {
            /* Worker thread, unknown descriptor — NPC. */
            t_isNpcEval = true;

            static std::atomic<bool> s_loggedNpc{false};
            if (!s_loggedNpc.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().trace(
                    "BaldFix: NPC eval on worker thread (desc 0x{:X})", desc);
        }

        auto result = s_originalCtxCreate(a1, a2, a3, a4, a5);

        t_isNpcEval = wasNpc;
        return result;
    }

    // ── PostfixEval hook ────────────────────────────────────────────────

    /**
     * @brief Hair-hiding suffix check.
     *
     * ruleObj+0x18 is a string handle (char**); double-deref for char*.
     * Hair-hiding suffixes: _a _c _d _f _i _q _v
     */
    static bool is_hair_hiding_rule(__int64 ruleObj) noexcept
    {
        auto handleAddr = *reinterpret_cast<const uintptr_t *>(ruleObj + 0x18);
        if (handleAddr < 0x10000)
            return false;
        auto suffixAddr = *reinterpret_cast<const uintptr_t *>(handleAddr);
        if (suffixAddr < 0x10000)
            return false;

        const auto *suffix = reinterpret_cast<const char *>(suffixAddr);
        if (suffix[0] != '_' || suffix[2] != '\0')
            return false;

        switch (suffix[1])
        {
        case 'a':
        case 'c':
        case 'd':
        case 'f':
        case 'i':
        case 'q':
        case 'v':
            return true;
        default:
            return false;
        }
    }

    static bool should_suppress_hair_hiding(__int64 ruleObj) noexcept
    {
        if (!is_hair_hiding_rule(ruleObj))
            return false;

        if (!is_category_hidden(Category::Helm) &&
            !is_category_hidden(Category::Cloak))
            return false;

        /* When PlayerOnly is enabled, skip if positively identified as NPC.
           The TLS flag is set by the context-create wrapper when the
           evaluation runs on a worker thread (NPC). Default is false
           (not NPC) so equip events and other main-thread paths suppress
           correctly without going through context-create. */
        if (flag_player_only().load(std::memory_order_relaxed) &&
            t_isNpcEval)
        {
            static std::atomic<bool> s_loggedSkip{false};
            if (!s_loggedSkip.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().trace(
                    "BaldFix: skipped NPC evaluation");
            return false;
        }

        return true;
    }

    __int64 __fastcall on_postfix_eval(__int64 ruleObj, __int64 context)
    {
        if (flag_bald_fix().load(std::memory_order_relaxed))
        {
            __try
            {
                if (should_suppress_hair_hiding(ruleObj))
                {
                    static std::atomic<bool> s_logged{false};
                    if (!s_logged.exchange(true, std::memory_order_relaxed))
                        DMK::Logger::get_instance().debug(
                            "BaldFix: suppressed hair-hiding rule");
                    return 0;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return s_originalPostfixEval(ruleObj, context);
    }

} // namespace EquipHide
