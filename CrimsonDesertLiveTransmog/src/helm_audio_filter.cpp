#include "helm_audio_filter.hpp"

#include "aob_resolver.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace DMK = DetourModKit;

namespace Transmog::HelmAudioFilter
{
    namespace
    {
        // 12-arg signature of the per-tag passive-skill registrar. a1 is the actor's skill registry, a2 a status int*,
        // a3 a u16 tag pointer, a4 the level, a5..a12 carry per-call context (descriptors, scope, etc.). The function
        // body returns `a2` and zeroes `*a2` on its natural exit. The suppress path replicates exactly that tail, so
        // callers cannot distinguish a SUPPRESS from a no-op registration.
        using PassiveSkillRegistrarFn = std::int32_t *(*)(std::int64_t, std::int32_t *, std::uint16_t *, std::int32_t,
                                                          std::int64_t, std::int64_t, char, std::int32_t, std::int64_t,
                                                          std::int64_t, char, std::int64_t);

        // Engine's u16-tag -> skill record resolver. It reads a3 as `*(u16*)a3` and indexes the SkillInfoManager entry
        // array at manager+0x58, then returns the record pointer. An out-of-range or unbound tag yields the manager's
        // default record instead. The returned record carries the per-level entry table at +0x18 and a level-count
        // vector at +0xF8. The entry-array displacement tracks the pa::StaticInfoManager2 layout and moves whenever
        // that base changes width, so verify it against live memory on patch day.
        using SkillTagResolverFn = std::int64_t (*)(std::uint16_t *);

        PassiveSkillRegistrarFn g_trampoline = nullptr;
        // Bound lazily on the first registrar invocation (see ensure_skill_tag_resolver). Atomic because arbitrary
        // equip threads publish and read it. Release/acquire gives a standards-clean happens-before, independent of
        // which thread won the bind.
        std::atomic<SkillTagResolverFn> g_skillTagResolver{nullptr};
        // The value stored in the first qword of every `pa::GameAudioEffectBuffData` instance: i.e. the address of
        // vfunc[0]. We compare buff_instance[0] against this value to determine class membership without RTTI walks.
        // Resolved at init via AOB on the vtable header (RTTI metadata ptr + first few vfuncs).
        std::uintptr_t g_gameAudioEffectVtable = 0;

        // Engine player static -- root of the chain that reaches the currently-controlled protagonist's Server CCOIA.
        // Used as a Kliff identity fallback during the save-load init race window, where Kliff's CharacterAssets struct
        // is not wired up yet but he IS the controlled actor at that moment (Kliff is always the first-spawned
        // protagonist).
        std::uintptr_t g_playerStatic = 0;

        std::atomic<bool> g_initDone{false};

        // Forward declaration. The definition sits at the bottom, next to the other scan logic.
        [[nodiscard]] std::uintptr_t resolve_skill_tag_resolver();

        // Binds g_skillTagResolver lazily on the first audio-classifier call. The bind is terminal.
        //
        // There is exactly one resolution path, and it must stay that way. The engine emits about a hundred
        // near-identical u16-tag resolvers, one per pa::*InfoManager. They are byte-identical for more than forty
        // bytes, including the whole prologue, the tag load, the bound check and the entry fetch. The only thing that
        // separates the SkillInfo resolver from its siblings is which manager global it loads, which lives in a
        // RIP-relative displacement. A byte signature therefore cannot identify this function without baking that
        // displacement, and a baked displacement stops matching on the next build that relocates the global. The scan
        // below matches the shared family shape and then picks the right member by reading the manager's RTTI class
        // name, which survives relocation.
        //
        // Resolution is deferred out of init() on purpose. The scan identifies the resolver through a live
        // pa::SkillInfoManager, and that singleton does not exist when init() runs, because the mod resolves its
        // anchors during the save-load window. The hooked registrar is a shared engine routine that also registers
        // character built-in passives, so the first detour call cannot guarantee a live singleton either. The scan
        // therefore repeats on every call until it binds. It runs on audio-classifier registrations, which happen on
        // equip and spawn, never per frame.
        //
        // noexcept: callers are noexcept and run on equip threads. The try wrapper swallows a log or allocation throw.
        // The compare-exchange publishes the pointer to acquire-loading readers regardless of whether logging throws
        // afterwards, and it guarantees a single winner if two equip threads resolve at the same time.
        void ensure_skill_tag_resolver() noexcept
        {
            if (g_skillTagResolver.load(std::memory_order_acquire) != nullptr)
                return; // already bound, terminal
            try
            {
                const std::uintptr_t addr = resolve_skill_tag_resolver();
                if (addr == 0)
                    return; // nothing resolved this call, retry on the next

                SkillTagResolverFn expected = nullptr;
                if (g_skillTagResolver.compare_exchange_strong(expected, reinterpret_cast<SkillTagResolverFn>(addr),
                                                               std::memory_order_release, std::memory_order_relaxed))
                    DMK::Logger::get_instance().info("[helm-audio] skill-tag resolver bound at 0x{:X}", addr);
            }
            catch (...)
            {
            }
        }

        // Chain from a pa::ServerChildOnlyInGameActor host to the CharacterAssets std::vector<std::string>:
        //
        //   host
        //     + 0x68 -> component table (64 packed pointer slots)
        //     + 0x40 -> slot[8] = pa::ServerCharacterControlActorComponent*
        //     + 0x40 -> (struct, no RTTI vtable in image)
        //     + 0x38 -> CharacterAssets vector base
        //
        // Slot[8] is the *Component, not a bare "...Actor": the "...Actor" type carries no RTTI in the image, so the
        // real polymorphic class is the server analog of the client CharacterControlActorComponent reached at
        // comp_table+0x40.
        //
        // CharacterAssets is a packed array of MSVC-style std::string entries (0x40-byte stride). Slot content per
        // protagonist:
        //
        //   Kliff (when armor-wearing, e.g. macduff):
        //     [0] 'character/appearance/.../cd_phm_macduff_00000.app_xml'
        //     [1] 'character/model/.../phm_01.pab'
        //     [2] 'character/binary/skeletonvariation/.../cd_phm_00_nude_*.pabc'
        //     [3] 'cd_portraitimage_Knowledge_Kliff'
        //     [4] 'player_1'
        //
        //   Oongka (no armor):
        //     [0] 'character/appearance/.../cd_phm_oongka_00000.app_xml'
        //     [1] 'character/binary/skeletonvariation/.../cd_pom_00_nude_*.pabc'
        //     [2] 'cd_portraitimage_Knowledge_Oongka'
        //     [3] 'player_3'
        //
        // Slot indices for the portrait / `player_N` strings differ between protagonists because armor-wearing actors
        // carry an extra `model` slot. The scan therefore walks all entries up to a small bound and short-circuits on
        // the first codename hit. Slot[0] (the .app_xml path) is the canonical match target for unarmored protagonists.
        // When armor renames slot[0], the portrait string at the slot whose index varies provides the fallback
        // codename.
        constexpr std::ptrdiff_t k_offComponentTable = 0x68;
        constexpr std::ptrdiff_t k_offCharCtlSlot = 0x40;
        constexpr std::ptrdiff_t k_offCharCtlToInner = 0x40;
        constexpr std::ptrdiff_t k_offInnerToAssets = 0x38;
        constexpr std::ptrdiff_t k_assetStride = 0x40;
        constexpr std::size_t k_assetMaxScan = 8;

        // Persistent per-host classification cache. Once the CharacterAssets scan identifies an actor's host pointer,
        // the result is stored, so later muffle events for the same actor do not have to re-walk the chain. The cache
        // also covers the save-load init race: Kliff's first muffle events arrive before the engine populates his
        // CharacterAssets struct, so the chain returns empty and the gate falls through. Later events for the same host
        // succeed once the struct is wired, and stay cached afterwards.
        std::mutex g_hostCacheMutex;
        std::unordered_map<std::uintptr_t, CDCore::ControlledCharacter> g_hostCache;

        // Per-entry std::string layout (MSVC SSO):
        //   +0x00 -> ptr (heap buffer when len>=16, else points into the entry's inline buffer at +0x10)
        //   +0x08  size_t length
        //   +0x10  inline buffer (16 bytes)
        //   +0x18  size_t capacity
        constexpr std::ptrdiff_t k_offEntryPtr = 0x00;
        constexpr std::ptrdiff_t k_offEntryLen = 0x08;
        constexpr std::ptrdiff_t k_offEntryInline = 0x10;
        constexpr std::size_t k_maxStringRead = 96;

        // Chain-walk reads route through DMK's SEH-guarded primitives (`DMKMemory::seh_read<T>` / `seh_read_bytes`).
        // They handle the low-address sentinel (<0x10000) internally and report a fault as an empty optional / `false`
        // return. The chain collapses to "not muffle" on any intermediate fault, because each step propagates the empty
        // optional via `value_or(0)`, and a 0 base terminates the next step at the sentinel check. The audio-classifier
        // tag pointer (`a3`) is a heap-allocated 8-byte record from the engine's iteminfo / skillinfo metadata. In
        // theory it stays live for the duration of the call, but the SEH guard keeps the feature alive on torn reads.

        // Structural identification of an audio-classifier registration call (a7 == 0 AND a3 in the
        // {u16 tag, u16 0, u16 lvl, u16 0} 8-byte-stride buffer layout the equip dispatcher uses). This cleanly
        // separates the audio-classifier code path from the other callers of the registrar (combat passives, teardown
        // nulls, vehicle skills, etc.) regardless of skill class.
        bool is_audio_classifier_call(std::uint16_t *a3, std::int32_t a4, char a7) noexcept
        {
            if (a7 != 0)
                return false;
            if (a3 == nullptr)
                return false;
            const auto a3Addr = reinterpret_cast<std::uintptr_t>(a3);
            const auto lvlEcho = DMKMemory::seh_read<std::uint16_t>(a3Addr + 4);
            if (!lvlEcho)
                return false;
            const auto padHi = DMKMemory::seh_read<std::uint16_t>(a3Addr + 2);
            if (!padHi || *padHi != 0u)
                return false;
            const auto padLo = DMKMemory::seh_read<std::uint16_t>(a3Addr + 6);
            if (!padLo || *padLo != 0u)
                return false;
            return *lvlEcho == static_cast<std::uint16_t>(a4);
        }

        // Chain walk that resolves the u16 tag at `*a3` to its skill record, walks to the first per-level entry, reads
        // the entry's vtable, and returns true iff the entry's class is `pa::GameAudioEffectBuffData`.
        //
        // Chain:
        //   record       = skill_tag_resolver(a3)             // engine resolver
        //   level_table  = *(record + 0x18)                   // per-level table
        //   inner_arr    = *(level_table + 0x00)              // first inner ptr
        //   first_entry  = *(inner_arr + 0x00)                // level 1 entry #1
        //   vtable       = *(first_entry + 0x00)              // class vtable
        //   return vtable == g_gameAudioEffectVtable
        //
        // Tag mapping in the live skillinfo catalog: only 0x64B (skill 91000, "PlateHelm_Audio") and 0x64C (skill
        // 91001, "PlateHelm_Audio_OpenableHelm") resolve to records whose per-level entry is
        // `pa::GameAudioEffectBuffData` (the class with description "투구 착용 시 먹먹한 소리" / Muffled sound when
        // wearing helmet). Other tags in the same neighborhood (0x647 / 0x64A / 0x650) resolve to
        // `pa::VoidPassiveBuffData` (item stat) or `pa::ImmuneBuffData` (sound-attack immunity) and pass through.
        // Iteminfo dump cross-check: the muffle set is the helms that equip skill 91000 or 91001. The chain walk
        // identifies them all without hardcoded tag tokens.
        bool is_audio_muffle_class(std::uint16_t *a3) noexcept
        {
            // Bind the resolver on first use. The bind retries while unbound. Until a bind holds, pass through.
            ensure_skill_tag_resolver();
            const auto resolver = g_skillTagResolver.load(std::memory_order_acquire);
            if (resolver == nullptr || g_gameAudioEffectVtable == 0 || a3 == nullptr)
                return false;

            // The resolver is engine code. In theory it is faultless, given the tag pointer's heap origin, but a torn
            // iteminfo refresh can publish a stale pointer mid-equip. The trampoline detour is reached from arbitrary
            // equip threads. Our own SEH frame around the call is cheaper than a proof of the engine's invariants, and
            // a fault here then passes through to the registrar instead of a crash.
            std::int64_t record = 0;
            __try
            {
                record = resolver(a3);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            if (record < 0x10000)
                return false;

            // record +0x18 -> +0x0 -> +0x0 -> +0x0 dereferences the per-level table, first inner ptr, level-1 entry,
            // then the entry's class vtable. seh_read_chain screens each link with plausible_userspace_ptr and reads
            // the terminal vtable value.
            const auto vtable =
                DMKMemory::seh_read_chain<std::uintptr_t>(static_cast<std::uintptr_t>(record), {0x18, 0x0, 0x0, 0x0})
                    .value_or(0);
            return vtable == g_gameAudioEffectVtable;
        }

        // ASCII copy with length cap. Walks the std::string entry's backing buffer in a single SEH-guarded bulk read.
        // The backing-buffer length comes from the std::string's own size field, which the caller already validated for
        // plausibility, so we trust it as the upper bound. The copy then truncates on the first embedded NUL, which
        // mirrors the C-string semantics the asset-path classifier expects.
        std::size_t copy_ascii_safe(std::uintptr_t src, std::size_t len, char *out, std::size_t cap) noexcept
        {
            if (src < 0x10000 || out == nullptr || cap == 0)
                return 0;
            const std::size_t limit = len < (cap - 1) ? len : (cap - 1);
            if (limit == 0)
            {
                out[0] = '\0';
                return 0;
            }
            if (!DMKMemory::seh_read_bytes(src, out, limit))
            {
                out[0] = '\0';
                return 0;
            }
            std::size_t actual = limit;
            for (std::size_t i = 0; i < limit; ++i)
            {
                if (out[i] == '\0')
                {
                    actual = i;
                    break;
                }
            }
            out[actual] = '\0';
            return actual;
        }

        // Delegates to CDCore::classify_appearance_by_path, which matches the appearance path against
        // dynamically-maintained codenames ("cd_phm_macduff" -> Kliff, "cd_phw_damian" -> Damiane, "cd_phm_oongka" ->
        // Oongka). CDCore's API keeps the codename set consistent with the rest of LT/EH/CDCore, including any runtime
        // auto-update of codenames the engine can perform.
        //
        // The function expects a path that contains "/cd_" as anchor. Protagonist .app_xml paths from the Server
        // CharacterAssets (slot 0) carry this anchor verbatim, e.g.
        // "character/appearance/1_pc/1_phm/cd_phm_macduff/...". Returns 1/2/3 for Kliff/Damiane/Oongka, 0 (Unknown)
        // else.
        CDCore::ControlledCharacter classify_asset_string(std::string_view sv) noexcept
        {
            const auto idx = CDCore::classify_appearance_by_path(sv);
            return static_cast<CDCore::ControlledCharacter>(idx);
        }

        // Reads one std::string entry from the CharacterAssets array. Handles both SSO (capacity <= 15, buffer inline
        // at +0x10) and heap (capacity > 15, ptr at +0x00). Returns chars copied (0 on fault / empty / implausible
        // length).
        std::size_t read_asset_entry(std::uintptr_t entry, char *out, std::size_t cap) noexcept
        {
            const auto len = DMKMemory::seh_read<std::uint32_t>(entry + k_offEntryLen).value_or(0);
            if (len == 0 || len > 0x10000)
                return 0;
            const auto ptr = DMKMemory::seh_read<std::uintptr_t>(entry + k_offEntryPtr).value_or(0);
            // SSO heuristic: if `ptr` looks like a canonical heap pointer, follow it. Otherwise read inline at +0x10.
            const bool ptrLooksValid = ptr >= 0x10000000000ULL && ptr < 0xF000000000000ULL;
            const auto src = ptrLooksValid ? ptr : (entry + k_offEntryInline);
            return copy_ascii_safe(src, len, out, cap);
        }

        // Walk a Server CCOIA host to its CharacterAssets vector and scan each entry (up to k_assetMaxScan) for a
        // protagonist codename. Returns the matched character or Unknown on chain fault / no match. `outMatchedAsset`
        // (if not null) receives the matched string for log diagnostics.
        CDCore::ControlledCharacter classify_host_by_assets(std::uintptr_t host, char *outMatchedAsset,
                                                            std::size_t outCap) noexcept
        {
            if (outMatchedAsset != nullptr && outCap > 0)
                outMatchedAsset[0] = '\0';
            if (host < 0x10000)
                return CDCore::ControlledCharacter::Unknown;

            // host +0x68 -> +0x40 -> +0x40 -> +0x38 dereferences the component table, character-controller slot, inner
            // record, then the CharacterAssets pointer. The trailing 0 forces the +0x38 link to be dereferenced, so the
            // result is the assets base that the slot-scan loop below indexes. Each link is screened by
            // plausible_userspace_ptr.
            const auto assets = DMKMemory::seh_resolve_chain(host, {k_offComponentTable, k_offCharCtlSlot,
                                                                    k_offCharCtlToInner, k_offInnerToAssets, 0})
                                    .value_or(0);
            if (assets < 0x10000)
                return CDCore::ControlledCharacter::Unknown;

            // Capture slot[0] (the appearance .app_xml) as the canonical diagnostic asset, shown in logs for every hit
            // regardless of which slot the codename match actually came from. This keeps log output consistent across
            // actors:
            //   Kliff (armored)  asset='cd_phm_macduff_*.app_xml'
            //   Oongka (base)    asset='cd_phm_oongka_*.app_xml'
            //   NPC              asset='character/appearance/.../<npc>.app_xml'
            // For protagonists the match is found independently in a later slot (portrait or player_N) when slot[0]
            // does not contain the codename (e.g. Kliff in custom armor).
            CDCore::ControlledCharacter matched = CDCore::ControlledCharacter::Unknown;
            for (std::size_t i = 0; i < k_assetMaxScan; ++i)
            {
                const auto entry = assets + i * k_assetStride;
                char buf[k_maxStringRead]{};
                const auto copied = read_asset_entry(entry, buf, sizeof(buf));
                if (copied == 0)
                    continue;
                if (i == 0 && outMatchedAsset != nullptr && outCap > 0)
                {
                    const auto wl = copied < (outCap - 1) ? copied : (outCap - 1);
                    std::memcpy(outMatchedAsset, buf, wl);
                    outMatchedAsset[wl] = '\0';
                }
                if (matched == CDCore::ControlledCharacter::Unknown)
                {
                    const std::string_view sv{buf, copied};
                    matched = classify_asset_string(sv);
                }
                if (matched != CDCore::ControlledCharacter::Unknown && outMatchedAsset != nullptr &&
                    outMatchedAsset[0] != '\0')
                {
                    // We have both a match and the slot[0] diag. Can short-circuit.
                    return matched;
                }
            }
            return matched;
        }

        // Walk the engine player-static chain to the currently-controlled protagonist's pa::ServerChildOnlyInGameActor.
        // The whole walk runs under one SEH frame via DMKMemory::seh_read_chain. Returns 0 on fault or pre-world.
        //
        //   *(g_playerStatic) -> root container
        //   *(root + 0x18)    -> pa::NwVirtualAsyncSession
        //   *(nwSes + 0xA0)   -> pa::ServerUserActor
        //   *(srvUA + 0xD0)   -> controlled host
        std::uintptr_t resolve_controlled_host() noexcept
        {
            if (g_playerStatic == 0)
                return 0;

            // The leading 0 dereferences g_playerStatic to the root container. Then +0x18 -> +0xA0 -> +0xD0 walks to
            // the controlled host and dereferences the +0xD0 link to read the host pointer value. Each link is screened
            // by plausible_userspace_ptr. A fault or an implausible link returns 0.
            const auto host =
                DMKMemory::seh_read_chain<std::uintptr_t>(g_playerStatic, {0x0, 0x18, 0xA0, 0xD0}).value_or(0);
            if (host < 0x10000)
                return 0;
            return host;
        }

        // Cached wrapper with Kliff init-race fallback. Resolution order:
        //   1. Per-host cache hit -> return cached identity.
        //   2. Asset-string scan -> on match, cache + return.
        //   3. Kliff fallback: if `host` equals the player-static chain leaf (i.e. host IS the currently-controlled
        //      actor) AND the asset scan returned Unknown, treat this as Kliff. Justification: Kliff is always the
        //      first-spawned protagonist and is the controlled actor at world load. During that brief window the engine
        //      did not wire up his CharacterAssets struct yet, so the scan returns empty. Damiane and Oongka spawn
        //      later with assets fully wired, so the scan succeeds for them without this fallback.
        //   4. Otherwise return Unknown.
        //
        // The cache stores ONLY successful identifications. The Kliff fallback caches its result too, so later muffle
        // events on the same Kliff host hit instantly without a re-walk of the chain.
        CDCore::ControlledCharacter classify_host_cached(std::uintptr_t host, char *outMatchedAsset,
                                                         std::size_t outCap) noexcept
        {
            if (outMatchedAsset != nullptr && outCap > 0)
                outMatchedAsset[0] = '\0';
            if (host < 0x10000)
                return CDCore::ControlledCharacter::Unknown;
            {
                std::lock_guard<std::mutex> lk(g_hostCacheMutex);
                const auto it = g_hostCache.find(host);
                if (it != g_hostCache.end())
                    return it->second;
            }
            const auto ch = classify_host_by_assets(host, outMatchedAsset, outCap);
            if (ch != CDCore::ControlledCharacter::Unknown)
            {
                std::lock_guard<std::mutex> lk(g_hostCacheMutex);
                g_hostCache.emplace(host, ch);
                return ch;
            }
            // The asset scan returned Unknown. Kliff init-race fallback: if the static chain reaches a host and that
            // host equals the one we are classifying, attribute it to Kliff.
            const auto controlled = resolve_controlled_host();
            if (controlled != 0 && controlled == host)
            {
                const auto fb = CDCore::ControlledCharacter::Kliff;
                if (outMatchedAsset != nullptr && outCap > 0)
                {
                    constexpr std::string_view marker = "<kliff-init-race-fallback>";
                    const auto wl = marker.size() < (outCap - 1) ? marker.size() : (outCap - 1);
                    std::memcpy(outMatchedAsset, marker.data(), wl);
                    outMatchedAsset[wl] = '\0';
                }
                std::lock_guard<std::mutex> lk(g_hostCacheMutex);
                g_hostCache.emplace(host, fb);
                return fb;
            }
            return CDCore::ControlledCharacter::Unknown;
        }

        // Inline detour for the per-tag passive-skill registrar. The call is suppressed when ALL three conditions hold:
        //   (1) Structural: the args identify the audio-classifier code path (a7 == 0 AND a3 in the {tag, 0, lvl, 0}
        //       layout the equip dispatcher uses).
        //   (2) Class chain walk: the resolved skill's first per-level entry has class `pa::GameAudioEffectBuffData`.
        //       This derives muffle-class membership from the engine's own RTTI without hardcoded tag tokens, so any
        //       future tag backed by the same class is admitted automatically.
        //   (3) Actor protagonist gate: the actor's CharacterAssets vector contains one of the configured protagonist
        //       codenames (Kliff, Damiane, or Oongka). The gate matches by appearance string, so a non-controlled
        //       protagonist actor (e.g. a sibling who sits next to the player) also has its voice muffle stripped.
        //
        // The combined gates restrict the suppress universe to protagonist-owned `pa::GameAudioEffectBuffData` entries.
        // Item-stat / sound-attack-immunity / generic combat passives all pass through.
        //
        // Log policy: SUPPRESS at INFO, so an audible behavior change is always present in the user log for triage.
        // The non-muffle and non-protagonist audio-classifier branches log at TRACE, so a user who flips
        // `log_level = trace` can see why a tag did or did not suppress.
        std::int32_t *__fastcall detour(std::int64_t a1, std::int32_t *a2, std::uint16_t *a3, std::int32_t a4,
                                        std::int64_t a5, std::int64_t a6, char a7, std::int32_t a8, std::int64_t a9,
                                        std::int64_t a10, char a11, std::int64_t a12) noexcept
        {
            // Fast reject: only the audio-classifier code path can ever trigger SUPPRESS. Every other call falls
            // through to the trampoline immediately. The is_audio_classifier check is cheap (4 SEH-wrapped u16 reads),
            // so it sits at the entry rather than after the protagonist walk, which keeps the unhooked-call cost
            // minimal.
            if (a3 == nullptr || a1 < 0x10000 || !is_audio_classifier_call(a3, a4, a7))
            {
                return g_trampoline(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
            }

            // Class chain walk: skip non-audio-muffle classes entirely (VoidPassiveBuffData, ImmuneBuffData, etc.).
            // Logged at TRACE because the audio-classifier path on a normal helm equip iterates 3-8 entries and most of
            // them fall here.
            const std::uint16_t tag =
                DMKMemory::seh_read<std::uint16_t>(reinterpret_cast<std::uintptr_t>(a3)).value_or(0);
            if (!is_audio_muffle_class(a3))
            {
                DMK::Logger::get_instance().trace("[helm-audio] non-muffle tag=0x{:X} lvl={}", tag, a4);
                return g_trampoline(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
            }

            // Muffle-class confirmed. Actor gate next: walk to the host's CharacterAssets std::vector<std::string> and
            // scan its entries for a protagonist codename literal (Kliff, Damiane, or Oongka). Returns the matched
            // character on first hit, pass-through otherwise.
            //
            // Why an asset-string scan and not another actor-identity source: the codename appears verbatim in the
            // resource paths shipped with the game data, so the match is grounded in semantically meaningful content
            // rather than a run-time-allocated handle or a build-specific byte offset on the host. The chain
            //   host -> +0x68 (component table)
            //        -> +0x40 (slot 8, CharacterControlActorComp)
            //        -> +0x40 -> +0x38 (CharacterAssets vector)
            // is populated once the CharacterControlActorComponent is wired, which precedes any audio-classifier
            // registration on the actor's skill list. The init-race window where the assets vector is still empty is
            // covered by the Kliff fallback inside classify_host_cached().
            const auto host = DMKMemory::seh_read<std::uintptr_t>(static_cast<std::uintptr_t>(a1) + 8).value_or(0);
            char matchedAsset[k_maxStringRead]{};
            const auto actorChar = classify_host_cached(host, matchedAsset, sizeof(matchedAsset));
            const bool isProtagonist = actorChar != CDCore::ControlledCharacter::Unknown;

            const auto actorName = CDCore::controlled_character_name(actorChar);
            const std::string_view actorSv = actorName.empty() ? std::string_view{"?"} : actorName;
            const std::string_view assetSv{matchedAsset};

            constexpr auto k_fmt = "[helm-audio] {} tag=0x{:X} lvl={} a1=0x{:X} "
                                   "host=0x{:X} actor='{}' asset='{}'";
            auto &log = DMK::Logger::get_instance();

            if (!isProtagonist)
            {
                // Muffle path, but no protagonist codename was found in the actor's CharacterAssets entries. Pass
                // through (NPCs, generic humanoids, pre-init).
                log.trace(k_fmt, "non-protagonist", tag, a4, static_cast<std::uintptr_t>(a1), host, actorSv, assetSv);
                return g_trampoline(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
            }

            // SUPPRESS. The tag never enters the actor's skill registry, so the audio dispatcher never finds it and no
            // muffle gets published for the protagonist. The single virtual call that the trampoline-skip leaves
            // un-invoked is rate-limited, role-gated, and refcount-balanced inside its own body (see header
            // bypass-safety analysis), so the skip is a no-op for the only role the protagonist ever has.
            log.info(k_fmt, "suppress", tag, a4, static_cast<std::uintptr_t>(a1), host, actorSv, assetSv);
            if (a2 != nullptr)
                *a2 = 0;
            return a2;
        }

        // Resolves the engine's u16-tag -> SkillInfo record resolver in a way meant to survive game updates rather than
        // pinning it to one build's addresses.
        //
        // The engine emits a family of byte-identical pa::*InfoManager index resolvers from one shared template. They
        // differ ONLY in the RIP-relative disp32 that names their manager-pointer global. A signature that bakes that
        // disp32 only matches the build it was captured from, because the linker recomputes that displacement on
        // every rebuild.
        //
        // This path instead signs ONLY the opcode/ModRM body shape (every movable operand wildcarded: the manager
        // disp32 and both forward-jump rel32s), then disambiguates the structurally identical hits at runtime by the
        // MSVC RTTI class name of the manager each resolver references. The class name is a source-level identity that
        // tends to outlive recompiles and address reshuffles, so this keeps working when the function or its manager
        // global moves. The SkillInfo resolver is the lowest-address member of the family, so the sweep usually returns
        // on the first hit. Correctness does not depend on that ordering, because every hit is RTTI-checked.
        [[nodiscard]] std::uintptr_t resolve_skill_tag_resolver()
        {
            auto &log = DMK::Logger::get_instance();

            // The two signature inputs -- the resolver's opcode body shape (every movable operand wildcarded) and the
            // target manager's decorated RTTI name -- live in aob_resolver.hpp (k_skillTagResolverBodyAob /
            // k_skillInfoManagerRttiName), next to the other candidate cascades, so all of this mod's signatures stay
            // in one place. This target has no fallback cascade: the RTTI pick is the only path. The body anchors at
            // function entry+0x12, and the offsets used below to read the manager disp32 and walk back to the entry are
            // named there too.
            const auto pattern = DMKScanner::parse_aob(::Transmog::k_skillTagResolverBodyAob);
            if (!pattern)
                return 0; // parse_aob already logged the malformed token

            const auto range = DMKMemory::host_module_range();
            if (!range.valid())
                return 0;

            const auto *cur = reinterpret_cast<const std::byte *>(range.base);
            const auto *const end = reinterpret_cast<const std::byte *>(range.end);
            std::size_t scanned = 0;

            while (cur < end)
            {
                const auto remaining = static_cast<std::size_t>(end - cur);
                const std::byte *hit = DMKScanner::find_pattern(cur, remaining, *pattern);
                if (hit == nullptr)
                    break;

                ++scanned;
                const auto match = reinterpret_cast<std::uintptr_t>(hit);

                // mov rbx,[rip+disp32]: the 3-byte movzx, then 48 8B 1D + disp32. disp32 sits at body offset
                // +DispOffset. The mov ends at +InstrEnd, which is the RIP base for it.
                const auto disp = DMKMemory::seh_read<std::int32_t>(match + ::Transmog::k_skillTagResolverDispOffset);
                if (disp.has_value())
                {
                    const auto mgrGlobal =
                        match + ::Transmog::k_skillTagResolverInstrEnd + static_cast<std::int64_t>(*disp);
                    // The pointer SLOT lives in the EXE image (.data/.bss). The manager object it points at is on the
                    // heap, and that object's vtable is back inside the image.
                    if (DMKMemory::contains(range, mgrGlobal))
                    {
                        const auto mgrObj = DMKMemory::seh_read<std::uintptr_t>(mgrGlobal).value_or(0);
                        const auto vtbl = DMKMemory::seh_read<std::uintptr_t>(mgrObj).value_or(0);
                        if (DMKRtti::vtable_is_type(vtbl, ::Transmog::k_skillInfoManagerRttiName))
                        {
                            const auto entry = match - ::Transmog::k_skillTagResolverEntryBackoff;
                            if (DMKScanner::is_likely_function_prologue(entry))
                            {
                                log.info("[helm-audio] skill-tag resolver resolved "
                                         "via RTTI '{}' at 0x{:X} (hit #{} of "
                                         "body-shape family)",
                                         ::Transmog::k_skillInfoManagerRttiName, entry, scanned);
                                return entry;
                            }
                        }
                    }
                }

                cur = hit + 1; // resume the sweep just past this hit
            }

            // Expected while the SkillInfoManager singleton is still constructing. ensure_skill_tag_resolver re-scans
            // on later calls, so a missing hit here is a trace, not a warning.
            log.trace("[helm-audio] RTTI scan: no SkillInfoManager resolver among "
                      "{} body-shape matches (will retry)",
                      scanned);
            return 0;
        }
    } // namespace

    bool init()
    {
        if (g_initDone.load(std::memory_order_acquire))
            return true;

        auto &log = DMK::Logger::get_instance();

        // Resolve the per-tag passive-skill registrar entry.
        const auto target = ::Transmog::resolve_address(::Transmog::k_helmAudioRegistrarCandidates,
                                                        "HelmAudioFilter_PassiveSkillRegistrar");
        if (target == 0)
        {
            log.warning("[helm-audio] registrar AOB resolve failed; "
                        "feature disabled");
            return false;
        }

        // The engine's tag -> skill record resolver binds lazily on the first registrar detour invocation rather than
        // here: its RTTI primary needs the pa::SkillInfoManager singleton to be live, which this init pass cannot
        // guarantee, but which skill registration (the hooked path) does guarantee. A resolve failure there only makes
        // is_audio_muffle_class pass through, so it never blocks hook installation. See ensure_skill_tag_resolver.

        // Resolve the pa::GameAudioEffectBuffData vtable. Each instance of that class stores its vtable base in its
        // first qword, so the filter compares buff_instance[0] against g_gameAudioEffectVtable to identify muffle-class
        // entries. The candidate cascade leads with a ResolveMode::RttiVtable tier (resolve by the patch-stable mangled
        // name, which self-heals across the vtable relocations that move the byte ctor-LEA anchors), then falls back to
        // those byte anchors. Both yield the same vtable base.
        const auto vtableAddr = ::Transmog::resolve_address(::Transmog::k_gameAudioEffectVtableCandidates,
                                                            "HelmAudioFilter_GameAudioEffectVtable");
        if (vtableAddr == 0)
        {
            log.warning("[helm-audio] GameAudioEffectBuffData vtable resolve failed "
                        "(RTTI name + AOB); feature disabled");
            return false;
        }
        g_gameAudioEffectVtable = vtableAddr;

        // Engine player static -- needed by the Kliff init-race fallback. On AOB failure we still install the hook. The
        // fallback then does not fire (the asset-string scan still works for Damiane/Oongka, and for Kliff once his
        // assets wire up).
        const auto playerStatic =
            ::Transmog::resolve_address(::Transmog::k_playerStaticCandidates, "HelmAudioFilter_PlayerStatic");
        if (playerStatic != 0)
            g_playerStatic = playerStatic;
        else
            log.warning("[helm-audio] player-static AOB resolve failed; "
                        "Kliff init-race fallback disabled (asset-string "
                        "scan still active)");

        // Install the inline hook on the registrar.
        auto &hookMgr = DMK::HookManager::get_instance();
        auto res =
            hookMgr.create_inline_hook("HelmAudioFilterPassiveSkillRegistrar", target,
                                       reinterpret_cast<void *>(&detour), reinterpret_cast<void **>(&g_trampoline));
        if (!res.has_value())
        {
            log.warning("[helm-audio] inline-hook install failed at 0x{:X}: "
                        "{}",
                        target, DetourModKit::Hook::error_to_string(res.error()));
            return false;
        }

        log.info("[helm-audio] inline-hook installed at 0x{:X} "
                 "(audio-vtable=0x{:X}, player-static=0x{:X}); "
                 "skill-tag resolver bound on first use",
                 target, vtableAddr, g_playerStatic);

        g_initDone.store(true, std::memory_order_release);
        return true;
    }

} // namespace Transmog::HelmAudioFilter
