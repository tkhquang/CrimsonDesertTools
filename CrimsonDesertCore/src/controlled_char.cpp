#include <cdcore/controlled_char.hpp>

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Controlled-character resolver.
//
// Static-chain + body-mesh asset-path resolver. See controlled_char.hpp
// for the architecture overview. This translation unit is intentionally
// narrow (no hooks, no learning caches, no broadcast subscriptions) --
// every query re-walks the chain through SEH-guarded reads.
//
// Internal state:
//   - s_cachedKliffCcoia: last-observed Kliff CCOIA pointer. Drives
//     world_generation() bump detection. The CCOIA sub-manager pointer
//     is persistent across save-load (its address never changes within
//     a process lifetime), so it cannot be used as the world-rebuild
//     signal. Kliff's CCOIA IS reallocated on every save-load --
//     tracking its address change is the correct signal for "world
//     rebuilt, drop session-local caches".
//   - s_worldGeneration: monotonic counter bumped each time the
//     Kliff CCOIA pointer changes.
// ---------------------------------------------------------------------------

namespace CDCore
{
    namespace
    {
        // Default static address (image-base + 0x5FA0430). The engine
        // writer is a single `mov cs:<rva>h, rdi` against this slot;
        // every published actor manager pointer flows through it.
        constexpr std::uintptr_t k_defaultPlayerBaseOffset = 0x5FA0430;

        // Lower bound for "looks like a heap pointer". Catches both real
        // null pointers and small enum-shaped values an uninitialised
        // slot may carry before the engine singleton is wired up.
        constexpr std::uintptr_t k_minValidPtr = 0x10000;

        // Static-chain offsets.
        // ClientUserActor.vec_data lives at userActor+0x78 and
        // ChildContainer.actor_list at childContainer+0x18; the
        // CCOIA identity dword sits at +0x60 with the category
        // marker in its high byte (+0x63). Sub-manager Kliff /
        // controlled slots (+0x30 / +0x38), the 16-byte vec[2] =
        // ChildContainer slot, the 100-entry actor-list capacity,
        // the 16-byte ptr+flag stride, and the live-flag value
        // 0x0101 round out the layout.
        constexpr std::ptrdiff_t k_offUserActor      = 0x28; // mgr +0x28
        constexpr std::ptrdiff_t k_offSubManager     = 0x08; // user +0x08
        constexpr std::ptrdiff_t k_offSubMgrKliff    = 0x30; // sub +0x30
        constexpr std::ptrdiff_t k_offSubMgrCtrl     = 0x38; // sub +0x38
        constexpr std::ptrdiff_t k_offUserVec        = 0x78; // user +0x78
        constexpr std::ptrdiff_t k_offVecChildSlot   = 0x20; // vec[2] (16B stride)
        constexpr std::ptrdiff_t k_offChildList      = 0x18; // child +0x18
        // Actor-list constants. The ChildContainer holds a 100-entry
        // list with 16-byte stride (ptr + live flag). The snapshot
        // path no longer uses this list -- it pulls protagonists
        // directly from ClientActorManager+0x100 -- but the
        // diagnostic walker debug_enumerate_actor_list keeps using
        // it.
        constexpr std::size_t    k_actorListCapacity = 100;
        constexpr std::size_t    k_actorListStride   = 16;

        // CCOIA identity bytes:
        //   +0x60 dword (LE-packed):
        //     byte +0x60: session-local actor ID (load order; varies)
        //     byte +0x63: 0xA0 = Kliff, 0xB0 = everyone else
        //
        // Direct protagonist lookup via ClientActorManager+0x100:
        //   - Pointer to an 8-byte-stride CCOIA-only actor array.
        //   - Position [0] = Kliff (always present).
        //   - Position [1] = Damiane CCOIA when she is loaded.
        //   - Position [2] = Oongka  CCOIA when she is loaded.
        //   - Positions [3+] are humanoid NPCs / followers in load
        //     order; rejected by the appearance-config classifier
        //     because their path does not contain any protagonist
        //     codename substring.
        constexpr std::ptrdiff_t k_offCcoiaIdentity   = 0x60;
        constexpr std::ptrdiff_t k_offMgrActorArray   = 0x100;
        constexpr std::uint8_t   k_kliffHighByte      = 0xA0;

        // Appearance-config asset-path chain. The CCOIA's body
        // component holder exposes a std::string carrying the
        // protagonist's appearance-config path; reading it yields a
        // character-specific internal codename embedded in the path:
        //   Kliff   -> ".../cd_phm_macduff/cd_phm_macduff_00000.app_xml"
        //   Damiane -> ".../cd_phw_damian/cd_phw_damian_00000.app_xml"
        //   Oongka  -> ".../cd_phm_oongka/cd_phm_oongka_00000.app_xml"
        // Path is bound at actor spawn; survives outfit changes, animation
        // transitions, and save-load. NPCs don't carry an appearance
        // config at this offset and fail the substring test.
        //
        // Chosen over the body-mesh path (which sits at the sibling
        // +0x28->+0x18 offsets) because the body-mesh string is
        // skeleton-archetype-keyed ("phw" identifies the female-
        // warrior skeleton, shared by any future female protagonist),
        // while the appearance codename is character-keyed and stays
        // unique even if the game adds a 4th protagonist sharing
        // Damiane's skeleton.
        constexpr std::ptrdiff_t k_offAppearChain1 = 0x68;
        constexpr std::ptrdiff_t k_offAppearChain2 = 0x40;
        constexpr std::ptrdiff_t k_offAppearChain3 = 0x40;
        constexpr std::ptrdiff_t k_offAppearChain4 = 0x38;

        // MSVC std::string: when content >= 16 chars the heap-buffer
        // pointer lives at +0x00 of the struct; smaller strings keep
        // inline content at the same offset. All known protagonist
        // appearance paths are 70+ chars (always heap-allocated) but
        // the resolver falls back to inline reading on the off-chance
        // the engine produces a short variant in a future update.
        constexpr std::ptrdiff_t  k_offStringBufPtr   = 0x00;
        constexpr std::size_t     k_appearPathReadMax = 160;
        constexpr std::uintptr_t  k_canonicalUpperPtr = 0x800000000000ULL;

        // Anchor: marks the start of the character subfolder name
        // in `.../cd_<archetype>_<codename>/...`. Codename search is
        // restricted to the suffix after this anchor so a path
        // component earlier in the tree that happens to contain a
        // codename substring (very unlikely, but defensive) cannot
        // cause a false positive.
        constexpr std::string_view k_appearAnchor = "/cd_";

        // Default character-subfolder substrings. Each protagonist's
        // appearance path embeds the full subfolder name twice
        // (subfolder + filename), so a plain substring search is
        // reliable. We default to the full `cd_<archetype>_<codename>`
        // form (not the bare codename) because:
        //   - it's self-documenting (a user reading the INI sees
        //     the asset-path shape directly),
        //   - it avoids any chance of a coincidental match against
        //     an unrelated path component that happens to contain
        //     a short codename like "damian",
        //   - the substring is still short enough that a user-side
        //     override can shorten it if they need a wider match.
        // The tokens are mutable at runtime via
        // set_protagonist_codenames() so a mod or game patch that
        // renames a subfolder can be patched without recompiling.
        // Guarded by s_codenameMutex; the read path snapshots all
        // three under one lock and releases before doing the search.
        constexpr std::string_view k_defaultCodenameKliff   = "cd_phm_macduff";
        constexpr std::string_view k_defaultCodenameDamiane = "cd_phw_damian";
        constexpr std::string_view k_defaultCodenameOongka  = "cd_phm_oongka";

        // -------------------------------------------------------------------
        // Mutable internal state.
        // -------------------------------------------------------------------

        // Track Kliff CCOIA pointer to drive world_generation() bumps
        // and kliff_low cache invalidation. The sub-manager pointer
        // is persistent across save-load and cannot be used here;
        // Kliff's CCOIA IS reallocated on every save-load, and its
        // session-local low byte at +0x60 shifts between sessions
        // (e.g., 0x01 in one session, 0x05 in another), so watching
        // the Kliff CCOIA pointer is the correct rebuild signal.
        std::atomic<std::uintptr_t> s_cachedKliffCcoia{0};
        std::atomic<std::uint64_t> s_worldGeneration{0};

        // Codename storage. Initialised to engine defaults; mutated
        // via set_protagonist_codenames() at config-load time and on
        // auto-reload.
        std::mutex   s_codenameMutex;
        std::string  s_codenameKliff{k_defaultCodenameKliff};
        std::string  s_codenameDamiane{k_defaultCodenameDamiane};
        std::string  s_codenameOongka{k_defaultCodenameOongka};

        // ---- SEH-guarded primitives -------------------------------------

        std::uintptr_t safe_read_qword(std::uintptr_t addr) noexcept
        {
            if (addr < k_minValidPtr)
                return 0;
            __try
            {
                return *reinterpret_cast<
                    const volatile std::uintptr_t *>(addr);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        std::uint32_t safe_read_dword(std::uintptr_t addr) noexcept
        {
            if (addr < k_minValidPtr)
                return 0;
            __try
            {
                return *reinterpret_cast<
                    const volatile std::uint32_t *>(addr);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        // ---- Player base resolution -------------------------------------

        std::uintptr_t resolve_player_base_address() noexcept
        {
            // Lazy one-shot: moduleBase + k_defaultPlayerBaseOffset.
            static std::atomic<std::uintptr_t> s_cachedDefault{0};
            const auto cached =
                s_cachedDefault.load(std::memory_order_acquire);
            if (cached >= k_minValidPtr)
                return cached;
            const auto mod = reinterpret_cast<std::uintptr_t>(
                GetModuleHandleW(nullptr));
            if (mod < k_minValidPtr)
                return 0;
            const auto addr = mod + k_defaultPlayerBaseOffset;
            s_cachedDefault.store(addr, std::memory_order_release);
            return addr;
        }

        // ---- Chain walks (SEH-guarded, single __try per call) -----------

        struct ChainAnchors
        {
            std::uintptr_t mgr        = 0; // pa::ClientActorManager
            std::uintptr_t userActor  = 0; // pa::ClientUserActor
            std::uintptr_t subMgr     = 0; // CCOIA sub-manager
            std::uintptr_t kliffCcoia = 0; // sub +0x30
            std::uintptr_t controlled = 0; // sub +0x38
        };

        ChainAnchors walk_chain_seh() noexcept
        {
            ChainAnchors out{};
            const auto playerBase = resolve_player_base_address();
            if (playerBase < k_minValidPtr)
                return out;
            __try
            {
                const auto mgr = *reinterpret_cast<
                    const volatile std::uintptr_t *>(playerBase);
                if (mgr < k_minValidPtr)
                    return out;
                const auto userActor = *reinterpret_cast<
                    const volatile std::uintptr_t *>(mgr + k_offUserActor);
                if (userActor < k_minValidPtr)
                    return out;
                const auto subMgr = *reinterpret_cast<
                    const volatile std::uintptr_t *>(
                    userActor + k_offSubManager);
                if (subMgr < k_minValidPtr)
                    return out;
                out.mgr        = mgr;
                out.userActor  = userActor;
                out.subMgr     = subMgr;
                out.kliffCcoia = *reinterpret_cast<
                    const volatile std::uintptr_t *>(
                    subMgr + k_offSubMgrKliff);
                out.controlled = *reinterpret_cast<
                    const volatile std::uintptr_t *>(
                    subMgr + k_offSubMgrCtrl);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // Partial state may be set; caller checks specific
                // anchors for non-zero before use.
            }
            return out;
        }

        // SEH-guarded resolution of the 100-entry actor list base from
        // a ClientUserActor. Walks userActor+0x78 (vec_data) -> +0x20
        // (vec[2] = ChildContainer ptr) -> +0x18 (actor list ptr).
        // Populates @p outVec / @p outChild / @p outList with the
        // intermediate anchors so callers can report them; returns 0
        // when any link is null/torn, otherwise the actor_list base.
        std::uintptr_t walk_to_actor_list_seh(
            std::uintptr_t userActor,
            std::uintptr_t &outVec,
            std::uintptr_t &outChild) noexcept
        {
            outVec = 0;
            outChild = 0;
            if (userActor < k_minValidPtr)
                return 0;
            __try
            {
                const auto vecData = *reinterpret_cast<
                    const volatile std::uintptr_t *>(
                    userActor + k_offUserVec);
                outVec = vecData;
                if (vecData < k_minValidPtr)
                    return 0;
                const auto childContainer = *reinterpret_cast<
                    const volatile std::uintptr_t *>(
                    vecData + k_offVecChildSlot);
                outChild = childContainer;
                if (childContainer < k_minValidPtr)
                    return 0;
                const auto actorList = *reinterpret_cast<
                    const volatile std::uintptr_t *>(
                    childContainer + k_offChildList);
                if (actorList < k_minValidPtr)
                    return 0;
                return actorList;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        // ---- World-generation tracking ----------------------------------

        // Update world-generation counter when Kliff CCOIA identity
        // changes (= save-load: the engine reallocates Kliff's CCOIA
        // with a fresh session-local +0x60 lo byte). Flushes the
        // kliff_low cache so the delta classifier reads the new
        // anchor rather than a stale value.
        void note_chain_observation(std::uintptr_t kliffCcoia) noexcept
        {
            if (kliffCcoia < k_minValidPtr)
                return;
            const auto last =
                s_cachedKliffCcoia.load(std::memory_order_acquire);
            if (kliffCcoia == last)
                return;
            std::uintptr_t expected = last;
            if (s_cachedKliffCcoia.compare_exchange_strong(
                    expected, kliffCcoia,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                s_worldGeneration.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        // ---- ASCII reader for std::string content ----------------------

        // Read up to (cap - 1) printable ASCII bytes from @p start into
        // @p buf, stopping at the first NUL. Returns false on torn read
        // or first non-printable byte (rejects garbage early so the
        // classifier doesn't pattern-match on partial pointer bytes).
        // The buffer is NUL-terminated on success.
        bool safe_read_ascii(std::uintptr_t start,
                             char *buf,
                             std::size_t cap,
                             std::size_t &outLen) noexcept
        {
            outLen = 0;
            if (start < k_minValidPtr || buf == nullptr || cap == 0)
                return false;
            __try
            {
                std::size_t i = 0;
                const auto limit = cap - 1;
                while (i < limit)
                {
                    const auto b = *reinterpret_cast<
                        const volatile std::uint8_t *>(start + i);
                    if (b == 0)
                        break;
                    if (b < 0x20 || b > 0x7E)
                        return false;
                    buf[i] = static_cast<char>(b);
                    ++i;
                }
                buf[i] = '\0';
                outLen = i;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Walk CCOIA +0x68 -> +0x40 -> +0x40 -> +0x38 to reach the
        // appearance-config std::string. Resolves heap-buffer vs
        // inline layout and returns the start address of ASCII
        // content; returns 0 on any torn link.
        std::uintptr_t resolve_appearance_path_buffer(
            std::uintptr_t ccoia) noexcept
        {
            if (ccoia < k_minValidPtr)
                return 0;
            auto p = safe_read_qword(ccoia + k_offAppearChain1);
            if (p < k_minValidPtr) return 0;
            p = safe_read_qword(p + k_offAppearChain2);
            if (p < k_minValidPtr) return 0;
            p = safe_read_qword(p + k_offAppearChain3);
            if (p < k_minValidPtr) return 0;
            const auto strStruct =
                safe_read_qword(p + k_offAppearChain4);
            if (strStruct < k_minValidPtr) return 0;
            const auto bufPtr =
                safe_read_qword(strStruct + k_offStringBufPtr);
            if (bufPtr >= k_minValidPtr &&
                bufPtr < k_canonicalUpperPtr)
                return bufPtr;
            return strStruct; // inline fallback
        }

        // ---- CCOIA classification ---------------------------------------

        // Classifies a CCOIA by reading its appearance-config path
        // and matching the embedded character codename. Returns
        // 1/2/3 for Kliff/Damiane/Oongka, or 0 when the chain is
        // unreachable or the path doesn't contain any known codename
        // (NPCs and follower humanoids fall here -- they either lack
        // an appearance config at this offset or carry an unknown
        // codename).
        //
        // We anchor on the "/cd_" substring (start of the character
        // subfolder name in `.../cd_<archetype>_<codename>/...`) and
        // search for the codename within the remaining suffix. This
        // avoids false positives if a codename's bytes appear earlier
        // in the path tree (e.g., directory names that coincide with
        // a substring of a character codename).
        std::uint32_t classify_by_appearance(
            std::uintptr_t ccoia) noexcept
        {
            const auto strStart = resolve_appearance_path_buffer(ccoia);
            if (strStart == 0)
                return 0;
            char buf[k_appearPathReadMax]{};
            std::size_t len = 0;
            if (!safe_read_ascii(strStart, buf, sizeof(buf), len))
                return 0;
            const std::string_view path{buf, len};
            const auto anchor = path.find(k_appearAnchor);
            if (anchor == std::string_view::npos)
                return 0;
            const auto suffix = path.substr(anchor);

            // Snapshot codenames under one lock, then release before
            // doing the substring search. Empty codenames are treated
            // as "skip this protagonist" rather than "match everything"
            // (find("") returns 0 = always-hit).
            std::string kliff, damiane, oongka;
            {
                std::lock_guard<std::mutex> lock(s_codenameMutex);
                kliff   = s_codenameKliff;
                damiane = s_codenameDamiane;
                oongka  = s_codenameOongka;
            }
            if (!kliff.empty() &&
                suffix.find(kliff) != std::string_view::npos)
                return 1;
            if (!damiane.empty() &&
                suffix.find(damiane) != std::string_view::npos)
                return 2;
            if (!oongka.empty() &&
                suffix.find(oongka) != std::string_view::npos)
                return 3;
            return 0;
        }

        // Primary CCOIA classifier. Tries the appearance-config path
        // first (character-codename identity that survives outfit and
        // state changes). Falls back to the +0x63 high-byte fast-path
        // for Kliff only when the appearance chain is mid-teardown
        // (engine save-load window where component pointers
        // transiently null). Damiane and Oongka are not distinguishable
        // without the appearance chain; the resolver returns Unknown
        // for them in that window rather than guessing.
        std::uint32_t classify_ccoia(std::uintptr_t ccoia) noexcept
        {
            if (ccoia < k_minValidPtr)
                return 0;
            const auto byAppearance = classify_by_appearance(ccoia);
            if (byAppearance != 0)
                return byAppearance;
            const auto packed =
                safe_read_dword(ccoia + k_offCcoiaIdentity);
            const auto highByte =
                static_cast<std::uint8_t>((packed >> 24) & 0xFFu);
            if (highByte == k_kliffHighByte)
                return 1;
            return 0;
        }

    } // namespace

    // ===================================================================
    // Public API.
    // ===================================================================

    std::uintptr_t current_controlled_ccoia() noexcept
    {
        const auto chain = walk_chain_seh();
        note_chain_observation(chain.kliffCcoia);
        return chain.controlled;
    }

    ControlledCharacter current_controlled_character() noexcept
    {
        const auto chain = walk_chain_seh();
        note_chain_observation(chain.kliffCcoia);
        if (chain.controlled < k_minValidPtr)
            return ControlledCharacter::Unknown;
        const auto idx = classify_ccoia(chain.controlled);
        switch (idx)
        {
        case 1: return ControlledCharacter::Kliff;
        case 2: return ControlledCharacter::Damiane;
        case 3: return ControlledCharacter::Oongka;
        default: return ControlledCharacter::Unknown;
        }
    }

    std::string_view controlled_character_name(
        ControlledCharacter ch) noexcept
    {
        switch (ch)
        {
        case ControlledCharacter::Kliff:   return "Kliff";
        case ControlledCharacter::Damiane: return "Damiane";
        case ControlledCharacter::Oongka:  return "Oongka";
        default:                           return {};
        }
    }

    std::string_view current_controlled_character_name() noexcept
    {
        return controlled_character_name(current_controlled_character());
    }

    std::uint32_t current_controlled_character_idx() noexcept
    {
        switch (current_controlled_character())
        {
        case ControlledCharacter::Kliff:   return 1;
        case ControlledCharacter::Damiane: return 2;
        case ControlledCharacter::Oongka:  return 3;
        default:                           return 0;
        }
    }

    std::uintptr_t equip_slot_for_ccoia(std::uintptr_t ccoia) noexcept
    {
        if (ccoia < k_minValidPtr)
            return 0;
        __try
        {
            const auto componentTable = *reinterpret_cast<
                const volatile std::uintptr_t *>(ccoia + 0x68);
            if (componentTable < k_minValidPtr)
                return 0;
            const auto equipSlot = *reinterpret_cast<
                const volatile std::uintptr_t *>(componentTable + 0x38);
            if (equipSlot < k_minValidPtr)
                return 0;
            return equipSlot;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    std::size_t snapshot_body_cache(BodyCacheEntry *out,
                                    std::size_t cap) noexcept
    {
        if (out == nullptr || cap == 0)
            return 0;
        const auto chain = walk_chain_seh();
        note_chain_observation(chain.kliffCcoia);
        if (chain.kliffCcoia < k_minValidPtr)
            return 0;

        std::size_t written = 0;
        // Kliff is always the first entry (always present).
        out[written++] = {chain.kliffCcoia, 1u};

        if (written >= cap)
            return written;

        // Walk the ClientActorManager+0x100 actor array. For each
        // non-Kliff entry, run the appearance-config classifier.
        // NPCs and followers fail the codename-substring match
        // (their appearance path does not contain `cd_phw_damian`
        // or `cd_phm_oongka`), so the loop emits at most one
        // Damiane and one Oongka entry.
        const auto actorArray =
            safe_read_qword(chain.mgr + k_offMgrActorArray);
        if (actorArray < k_minValidPtr)
            return written;

        bool foundDamiane = false;
        bool foundOongka  = false;
        for (std::size_t i = 0;
             i < k_actorListCapacity && written < cap;
             ++i)
        {
            const auto candidate = safe_read_qword(
                actorArray + i * sizeof(std::uintptr_t));
            if (candidate < k_minValidPtr ||
                candidate == chain.kliffCcoia)
                continue;
            const auto idx = classify_ccoia(candidate);
            if (idx == 2 && !foundDamiane)
            {
                out[written++] = {candidate, 2u};
                foundDamiane = true;
            }
            else if (idx == 3 && !foundOongka)
            {
                out[written++] = {candidate, 3u};
                foundOongka = true;
            }
            if (foundDamiane && foundOongka)
                break;
        }

        return written;
    }

    ActorListDebugSummary debug_enumerate_actor_list(
        ActorListDebugEntry *out, std::size_t cap) noexcept
    {
        ActorListDebugSummary summary{};
        if (out == nullptr || cap == 0)
            return summary;

        const auto chain = walk_chain_seh();
        note_chain_observation(chain.kliffCcoia);
        summary.mgr        = chain.mgr;
        summary.userActor  = chain.userActor;
        summary.subMgr     = chain.subMgr;
        summary.kliffCcoia = chain.kliffCcoia;
        summary.controlled = chain.controlled;

        // Reuse the shared chain-to-list walker. The diagnostic
        // summary just publishes the intermediate anchors the walker
        // already collects.
        const auto actorList = walk_to_actor_list_seh(
            chain.userActor, summary.vecData, summary.childContainer);
        summary.actorList = actorList;
        if (actorList < k_minValidPtr)
            return summary;

        __try
        {
            std::size_t n = 0;
            for (std::size_t i = 0;
                 i < k_actorListCapacity && n < cap;
                 ++i)
            {
                const auto entryBase =
                    actorList + i * k_actorListStride;
                const auto ccoia = *reinterpret_cast<
                    const volatile std::uintptr_t *>(entryBase);
                const auto flag = *reinterpret_cast<
                    const volatile std::uint64_t *>(entryBase + 8);
                if (ccoia < k_minValidPtr && flag == 0)
                    continue;
                std::uint32_t ident = 0;
                if (ccoia >= k_minValidPtr)
                    ident = safe_read_dword(ccoia + k_offCcoiaIdentity);
                out[n].ccoia    = ccoia;
                out[n].flag     = flag;
                out[n].identity = ident;
                ++n;
            }
            summary.rawEntries = n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Partial state; return what we have so far.
        }
        return summary;
    }

    std::uint64_t world_generation() noexcept
    {
        // Take an opportunistic chain snapshot to refresh the counter
        // when the caller hasn't recently queried identity. The
        // counter itself is monotonic and never regresses on a
        // transient null window.
        const auto chain = walk_chain_seh();
        note_chain_observation(chain.kliffCcoia);
        return s_worldGeneration.load(std::memory_order_acquire);
    }

    void invalidate_controlled_character() noexcept
    {
        s_cachedKliffCcoia.store(0, std::memory_order_release);
    }

    void set_protagonist_codenames(std::string_view kliff,
                                   std::string_view damiane,
                                   std::string_view oongka) noexcept
    {
        std::lock_guard<std::mutex> lock(s_codenameMutex);
        if (!kliff.empty())   s_codenameKliff   = kliff;
        if (!damiane.empty()) s_codenameDamiane = damiane;
        if (!oongka.empty())  s_codenameOongka  = oongka;
    }

} // namespace CDCore
