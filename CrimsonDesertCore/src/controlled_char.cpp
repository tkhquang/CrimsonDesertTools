#include "cdcore/controlled_char.hpp"
#include "cdcore/anchors.hpp"

#include <Windows.h>

#include <DetourModKit/logger.hpp>
#include <DetourModKit/memory.hpp>
#include <DetourModKit/rtti.hpp>
#include <DetourModKit/rtti_dissect.hpp>
#include <DetourModKit/scan.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

// Controlled-character resolver.
//
// Static-chain + body-mesh asset-path resolver. See controlled_char.hpp for the architecture overview. This translation
// unit is intentionally narrow (no hooks, no learning caches, no broadcast subscriptions) - every query re-walks the
// chain through SEH-guarded reads.
//
// Internal state:
//   - s_cached_kliff_ccoia: last-observed Kliff CCOIA pointer. Drives world_generation() bump detection. The CCOIA
//     sub-manager pointer is persistent across save-load (its address never changes within a process lifetime), so it
//     cannot be used as the world-rebuild signal. Kliff's CCOIA IS reallocated on every save-load, so its address
//     change is the correct signal for "world rebuilt, drop session-local caches".
//   - s_world_generation: monotonic counter bumped each time the Kliff CCOIA pointer changes.

namespace CDCore
{
    // INI-tunable self-heal landmark window (search radius per side, bytes), bound to [Advanced] SelfHealWindow by each
    // consumer. 0x200 covers ~10x the worst historical manager drift (+0x30) with margin. The manager references
    // pa::ClientUserActor exactly once (every neighboring slot is a different actor/container type or non-polymorphic,
    // and the next pointer to the user actor lives megabytes away), so the window is decoy-free across its whole range
    // - the size is a realistic-re-layout-reach choice, not a decoy bound. Raise it via the INI toward MAX_HEAL_WINDOW
    // only if a patch shifts pa::ClientUserActor further from the nominal slot than this.
    inline constexpr int HEAL_WINDOW_DEFAULT = 0x200;

    std::atomic<int> &heal_window_setting() noexcept
    {
        static std::atomic<int> s_heal_window{HEAL_WINDOW_DEFAULT};
        return s_heal_window;
    }

    namespace
    {
        // Static-chain offsets. ClientUserActor.vec_data lives at user_actor+0x78 and ChildContainer.actor_list at
        // child_container+0x18. The CCOIA identity dword sits at +0x60 with the category marker in its high byte
        // (+0x63). Sub-manager Kliff / controlled slots (+0x30 / +0x38), the 16-byte vec[2] = ChildContainer slot, the
        // 100-entry actor-list capacity, the 16-byte ptr+flag stride, and the live-flag value 0x0101 round out the
        // layout.
        // mgr -> user_actor offset, with runtime self-heal. The seed comes from controlled_char.hpp. rtti_dissect
        // re-resolves mgr->user_actor from the live pa::ClientActorManager on the first walk, so a future manager
        // re-layout self-corrects. heal_landmark checks the nominal slot first, so an unshifted binary short-circuits
        // and the seed stays.
        //
        // The actor-array descriptor is healed SEPARATELY (see heal_mgr_actor_array), NOT from this offset's healed
        // delta. Both live in the post-header region, but they move independently: the descriptor block can shift
        // while the user_actor slot stays put, in which case a derived offset is silently wrong even though the
        // user_actor heal reports success. A derivation of one offset from another couples two independent layout
        // facts and downgrades a detectable miss into a wrong answer, so each one resolves against the live object on
        // its own evidence.
        constexpr std::ptrdiff_t OFF_USER_ACTOR_NOMINAL = actor_chain_offsets::ACTOR_MANAGER_TO_USER_ACTOR;
        constexpr std::string_view USER_ACTOR_MANGLED = ".?AVClientUserActor@pa@@";

        std::atomic<std::ptrdiff_t> s_off_user_actor{OFF_USER_ACTOR_NOMINAL};
        std::atomic<bool> s_chain_offsets_healed{false};
        std::atomic<int> s_chain_heal_attempts{0};

        // Actor-array descriptor. The manager keeps a run of uniform 16-byte records, each one
        //
        //     +0x00 qword  pointer to an 8-byte-stride CCOIA array
        //     +0x08 u32    live element count
        //     +0x0C u32    element capacity
        //
        // and the protagonist lookup wants the DENSEST of them (see heal_mgr_actor_array for how it is picked out).
        // The nominal seed is the last verified location and is only a fallback: heal_mgr_actor_array re-derives it
        // from the live object.
        constexpr std::ptrdiff_t MGR_ARRAY_COUNT_REL = 0x08;
        constexpr std::ptrdiff_t MGR_ARRAY_CAP_REL = 0x0C;
        constexpr std::ptrdiff_t OFF_MGR_ACTOR_ARRAY_NOMINAL = 0x128;

        // Byte window of the manager object searched for the descriptor. Wide enough to cover the whole run of
        // records plus room for the header to grow, small enough that a miss costs one bounded sweep.
        constexpr std::ptrdiff_t MGR_ARRAY_HEAL_WINDOW = 0x400;

        std::atomic<std::ptrdiff_t> s_off_mgr_actor_array{OFF_MGR_ACTOR_ARRAY_NOMINAL};
        std::atomic<bool> s_mgr_array_healed{false};
        std::atomic<int> s_mgr_array_heal_attempts{0};

        // Resolved self-heal window: the [Advanced] SelfHealWindow value clamped to the DMK maximum. A non-positive
        // (unset) value falls back to the default. Read once per heal attempt, never on a hot path.
        [[nodiscard]] std::size_t heal_window() noexcept
        {
            const int configured = heal_window_setting().load(std::memory_order_relaxed);
            if (configured <= 0)
                return static_cast<std::size_t>(HEAL_WINDOW_DEFAULT);
            const auto w = static_cast<std::size_t>(configured);
            return w > DMK::rtti::MAX_HEAL_WINDOW ? DMK::rtti::MAX_HEAL_WINDOW : w;
        }

        [[nodiscard]] std::ptrdiff_t off_user_actor() noexcept
        {
            return s_off_user_actor.load(std::memory_order_acquire);
        }

        [[nodiscard]] std::ptrdiff_t off_mgr_actor_array() noexcept
        {
            return s_off_mgr_actor_array.load(std::memory_order_acquire);
        }

        // Capacity lives inside the SAME descriptor record as the array pointer, at a fixed intra-record offset, so it
        // follows the healed pointer offset by construction. A within-record relationship is safe to derive. A
        // cross-region one is not, because the two regions can move independently.
        [[nodiscard]] std::ptrdiff_t off_mgr_actor_array_cap() noexcept
        {
            return off_mgr_actor_array() + MGR_ARRAY_CAP_REL;
        }

        /// pa::ClientActorManager, the object the ClientActorManagerGlobal slot publishes.
        constexpr std::string_view ACTOR_MANAGER_MANGLED = ".?AVClientActorManager@pa@@";
        std::atomic<bool> s_manager_type_checked{false};

        // One-time identity check on the published manager. The slot ladder proves where the engine STORES the
        // manager, not what the slot holds: a slot that moved by one pointer on a patch still yields a plausible heap
        // pointer, and the walk then reads a neighboring object's fields as manager fields without ever faulting. The
        // manager's own RTTI names its class, so a wrong slot reports itself once here instead of surfacing later as
        // "companions lose transmog". Latches only once a vtable pointer is readable, because the manager may not
        // exist yet during early startup, and the walk retries on its own.
        void check_manager_type(std::uintptr_t mgr) noexcept
        {
            if (s_manager_type_checked.load(std::memory_order_acquire))
                return;
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{mgr});
            if (!vtable || !DMK::memory::is_plausible_ptr(DMK::Address{*vtable}))
                return;
            s_manager_type_checked.store(true, std::memory_order_release);
            if (DMK::rtti::vtable_is_type(DMK::Address{*vtable}, ACTOR_MANAGER_MANGLED))
            {
                DMK::log().debug("ClientActorManagerGlobal pointee verified as {}", ACTOR_MANAGER_MANGLED);
                return;
            }
            DMK::log().warning(
                "ClientActorManagerGlobal pointee {:#x} is not {} (vtable {:#x}). The slot ladder resolved a "
                "neighboring global, and the player chain is not trustworthy on this build",
                mgr,
                ACTOR_MANAGER_MANGLED,
                *vtable
            );
        }

        // Re-entrant offset self-heal. Latches only on success (or the no-drift short-circuit inside heal_landmark).
        // Until the player chain is wired the heal legitimately finds nothing: a user can sit at the main menu, where
        // no pa::ClientUserActor exists yet, for any length of time. So it NEVER gives up and NEVER latches on failure
        // - every walk retries until the fully-wired chain heals (this also avoids the interner-hook cold-load latch
        // bug). The nominal seeds carry the meantime, and the walk's own plausibility gate rejects a garbage
        // dereference, so an unhealed state can never mis-walk. A retry cap is therefore wrong here.
        void heal_chain_offsets(std::uintptr_t mgr_base) noexcept
        {
            if (s_chain_offsets_healed.load(std::memory_order_acquire))
                return;

            DMK::rtti::Landmark lm{};
            lm.base = DMK::Address{mgr_base};
            lm.nominal_offset = OFF_USER_ACTOR_NOMINAL;
            lm.window = heal_window();
            lm.expected_mangled = std::string{USER_ACTOR_MANGLED};
            // mgr+0x58 is a qword POINTER to a single-COL pa::ClientUserActor (COL offset 0, no multiple inheritance),
            // so PointerToObject is the exact slot shape.
            lm.indirection = DMK::rtti::Indirection::PointerToObject;

            const auto hit = DMK::rtti::heal_landmark(lm);
            if (!hit.has_value())
            {
                // NoMatch / Ambiguous / BadDescriptor: keep the nominal seeds and retry on the next walk. NoMatch is
                // expected and benign before a save is loaded. Never publish a guessed offset. Surface a persistent
                // failure only on a geometric schedule (every power-of-two walk) at DEBUG, so a real patch-day drift
                // stays diagnosable with no flood of the per-walk path and no alarm on the normal main-menu wait.
                const int n = s_chain_heal_attempts.fetch_add(1, std::memory_order_acq_rel) + 1;
                if ((n & (n - 1)) == 0)
                    DMK::log().debug(
                        "ActorChainOffsets not healed yet after {} walk(s) ({}); using nominal "
                        "offsets, will keep retrying",
                        n,
                        hit.error().message()
                    );
                return;
            }

            const std::ptrdiff_t healed = hit->healed_offset;
            const std::ptrdiff_t drift = healed - OFF_USER_ACTOR_NOMINAL;
            // A walk that observes the latch must also observe the healed offset, so both stores release.
            s_off_user_actor.store(healed, std::memory_order_release);
            s_chain_offsets_healed.store(true, std::memory_order_release);
            // One-shot confirmation that the rtti_dissect path engaged and the mgr->user_actor slot reverse-resolved to
            // pa::ClientUserActor. A nonzero drift is a manager re-layout that self-corrected on a patch. Surface it
            // as a WARNING so the offset change is easy to spot.
            if (drift != 0)
                DMK::log().warning(
                    "ActorChainOffsets DRIFTED: mgr->userActor {:#x} nominal {:#x} drift {} - "
                    "self-healed (manager layout changed)",
                    healed,
                    OFF_USER_ACTOR_NOMINAL,
                    drift
                );
            else
                DMK::log().info("ActorChainOffsets self-heal OK: mgr->userActor {:#x} (matches nominal)", healed);
        }

        /// Offset from pa::ClientUserActor to the CCOIA sub-manager.
        constexpr std::ptrdiff_t OFF_SUB_MANAGER = 0x08;
        /// Offset from the CCOIA sub-manager to the Kliff CCOIA.
        constexpr std::ptrdiff_t OFF_SUB_MGR_KLIFF = 0x30;
        /// Offset from the CCOIA sub-manager to the controlled CCOIA.
        constexpr std::ptrdiff_t OFF_SUB_MGR_CTRL = 0x38;
        /// Offset from pa::ClientUserActor to the component-vector data pointer.
        constexpr std::ptrdiff_t OFF_USER_VEC = 0x78;
        /// Offset of the ChildContainer slot inside the component vector (vec[2], 16-byte stride).
        constexpr std::ptrdiff_t OFF_VEC_CHILD_SLOT = 0x20;
        /// Offset from the ChildContainer to the actor list.
        constexpr std::ptrdiff_t OFF_CHILD_LIST = 0x18;

        // Actor-list constants. The ChildContainer holds a 100-entry list with 16-byte stride (ptr + live flag). Only
        // the diagnostic walker debug_enumerate_actor_list reads this list. The snapshot path reads the manager's
        // actor-array descriptor instead.
        constexpr std::size_t ACTOR_LIST_CAPACITY = 100;
        constexpr std::size_t ACTOR_LIST_STRIDE = 16;

        // CCOIA identity bytes:
        //   +0x60 dword (LE-packed):
        //     byte +0x60: session-local actor ID (load order, varies)
        //     byte +0x63: 0xA0 = Kliff, 0xB0 = everyone else
        //
        // Direct protagonist lookup through the manager's actor-array descriptor. The array is a DENSE actor vector:
        // every loaded character - protagonists, companions, AND every humanoid NPC - is appended in spawn order.
        // Protagonists do NOT cluster at the front. In a crowded scene (e.g. a large NPC battle) Kliff sits at [0] but
        // the other protagonists can land hundreds of entries deep, interleaved with NPCs. The scan must therefore
        // cover the whole live extent rather than a fixed prefix. A too-small bound silently drops protagonists that
        // spawn late. Non-protagonist entries are rejected by the appearance-config classifier (their path carries no
        // protagonist codename).
        constexpr std::ptrdiff_t OFF_CCOIA_IDENTITY = 0x60;
        constexpr std::uint8_t KLIFF_HIGH_BYTE = 0xA0;

        // Scan bound for the actor array. The snapshot reads the engine's own capacity field on every call so the
        // bound tracks the array as it grows, then clamps it to a sane window: a read below the floor counts as
        // torn/garbage and falls back to a generous fixed bound, while a read above the ceiling is clamped down so
        // worst-case work stays bounded. The snapshot runs at ~1 Hz off background poll threads, so even a
        // full-extent sweep is cheap.
        //
        // The bound comes from the CAPACITY field, never the count: the engine clears the count and refills it as it
        // rebuilds the list each tick, so a count read taken at an arbitrary moment is legitimately 0 on a fully
        // populated world.
        constexpr std::uint32_t MGR_ARRAY_CAP_MIN = 16;
        constexpr std::uint32_t MGR_ARRAY_CAP_HARD_CAP = 8192;
        constexpr std::size_t MGR_ARRAY_SCAN_FALLBACK = 1024;

        // Structural self-heal for the actor-array descriptor. It resolves against the live manager instead of a
        // derivation from the user_actor heal.
        //
        // Identification: sweep the manager for a record whose pointer slot is a plausible pointer, whose capacity is
        // in range, whose count does not exceed that capacity, and whose array holds the known Kliff CCOIA at index 0.
        // Several sibling lists satisfy all of that (the engine keeps per-system subsets that also lead with Kliff),
        // so the widest capacity wins: the dense vector is by construction a superset of the filtered ones, and a
        // subset silently drops a companion that it does not track.
        //
        // Cost is one bounded sweep of the window per call until it latches. Callers already hold the walked chain, so
        // the Kliff pointer needed as the discriminator is free.
        //
        // Never gives up and never latches on failure. A player can sit at the main menu with no chain wired for any
        // length of time, and the lists are also momentarily empty mid-rebuild, so a miss is normal and the next walk
        // retries. The nominal seed carries the meantime.
        void heal_mgr_actor_array(std::uintptr_t mgr_base, std::uintptr_t kliff_ccoia) noexcept
        {
            if (s_mgr_array_healed.load(std::memory_order_acquire))
                return;
            if (!DMK::memory::is_plausible_ptr(DMK::Address{mgr_base}) ||
                !DMK::memory::is_plausible_ptr(DMK::Address{kliff_ccoia}))
                return;

            std::ptrdiff_t best_off = 0;
            std::uint32_t best_cap = 0;
            for (std::ptrdiff_t off = 0; off <= MGR_ARRAY_HEAL_WINDOW;
                 off += static_cast<std::ptrdiff_t>(sizeof(std::uintptr_t)))
            {
                const auto arr = DMK::memory::read<std::uintptr_t>(DMK::Address{mgr_base + off}).value_or(0);
                if (!DMK::memory::is_plausible_ptr(DMK::Address{arr}))
                    continue;
                const auto cap =
                    DMK::memory::read<std::uint32_t>(DMK::Address{mgr_base + off + MGR_ARRAY_CAP_REL}).value_or(0);
                if (cap < MGR_ARRAY_CAP_MIN || cap > MGR_ARRAY_CAP_HARD_CAP || cap <= best_cap)
                    continue;
                const auto count =
                    DMK::memory::read<std::uint32_t>(DMK::Address{mgr_base + off + MGR_ARRAY_COUNT_REL}).value_or(0);
                if (count > cap)
                    continue;
                if (DMK::memory::read<std::uintptr_t>(DMK::Address{arr}).value_or(0) != kliff_ccoia)
                    continue;
                best_cap = cap;
                best_off = off;
            }

            if (best_cap == 0)
            {
                // Surface a persistent failure on a geometric schedule (every power-of-two attempt) at DEBUG, so a
                // real layout change stays diagnosable with no flood of the per-walk path.
                const int n = s_mgr_array_heal_attempts.fetch_add(1, std::memory_order_acq_rel) + 1;
                if ((n & (n - 1)) == 0)
                    DMK::log().debug(
                        "MgrActorArray not healed yet after {} walk(s) (no descriptor in the window leads with the "
                        "Kliff CCOIA); using nominal offset {:#x}, will keep retrying",
                        n,
                        OFF_MGR_ACTOR_ARRAY_NOMINAL
                    );
                return;
            }

            // A snapshot that observes the latch must also observe the healed offset, so both stores release.
            s_off_mgr_actor_array.store(best_off, std::memory_order_release);
            s_mgr_array_healed.store(true, std::memory_order_release);
            if (best_off != OFF_MGR_ACTOR_ARRAY_NOMINAL)
                DMK::log().warning(
                    "MgrActorArray DRIFTED: descriptor {:#x} nominal {:#x} drift {} cap {} - self-healed (manager "
                    "layout changed)",
                    best_off,
                    OFF_MGR_ACTOR_ARRAY_NOMINAL,
                    best_off - OFF_MGR_ACTOR_ARRAY_NOMINAL,
                    best_cap
                );
            else
                DMK::log()
                    .info("MgrActorArray self-heal OK: descriptor {:#x} cap {} (matches nominal)", best_off, best_cap);
        }

        // Appearance-config asset-path chain. The CCOIA's body component holder exposes a std::string that carries
        // the protagonist's appearance-config path, and a read of that path yields a character-specific internal
        // codename embedded in it:
        //   Kliff   -> ".../cd_phm_macduff/cd_phm_macduff_00000.app_xml"
        //   Damiane -> ".../cd_phw_damian/cd_phw_damian_00000.app_xml"
        //   Oongka  -> ".../cd_phm_oongka/cd_phm_oongka_00000.app_xml"
        // The path is bound at actor spawn and survives outfit changes, animation transitions, and save-load. NPCs do
        // not carry an appearance config at this offset and fail the substring test.
        //
        // Chosen over the body-mesh path (which sits at the sibling +0x28->+0x18 offsets) because the body-mesh string
        // is skeleton-archetype-keyed ("phw" identifies the female-warrior skeleton, shared by any future female
        // protagonist), while the appearance codename is character-keyed and stays unique even if the game adds a 4th
        // protagonist that shares Damiane's skeleton.
        //
        // Step-2 caveat: the holder at ccoia+0x68 is a dense component pointer table that grows as actor components
        // register, so the target component (pa::ClientCharacterControlActorComponent) lands at a different slot index
        // that depends on how many components the engine wired up when the resolver reads it. Observed Kliff layouts:
        //
        //   early load (~5 components present):
        //     ccoia +0x68 -> p1
        //       p1 +0x40 -> pa::ClientEquipSlotActorComponent (wrong branch)
        //       p1 +0x48 -> pa::ClientCharacterControlActorComponent
        //
        //   later load (~22 components present):
        //     ccoia +0x68 -> p1
        //       p1 +0x38 -> pa::ClientEquipSlotActorComponent
        //       p1 +0x40 -> pa::ClientCharacterControlActorComponent
        //       p1 +0x48 -> pa::ClientVehicleActorComponent
        //
        // Step 2 (locate the CCC inside p1) walks the table by RTTI/vtable rather than fixed offset, so the chain is
        // session-stable regardless of how many components register. The walk runs through
        // `DMK::rtti::find_in_pointer_table` against `CCC_TYPE_DESCRIPTOR_NAME`. Steps 3 (+0x40) and 4 (+0x38)
        // dereference fixed members of the resolved CCC and are structurally stable.
        constexpr std::ptrdiff_t OFF_APPEAR_CHAIN1 = 0x68;
        constexpr std::ptrdiff_t OFF_APPEAR_CHAIN3 = 0x40;
        constexpr std::ptrdiff_t OFF_APPEAR_CHAIN4 = 0x38;

        // The equip-slot component is pinned to a fixed slot of the same component table, so it needs no RTTI walk.
        constexpr std::ptrdiff_t OFF_EQUIP_SLOT_COMPONENT = 0x38;

        // RTTI mangled-name string of the target component class. MSVC encodes class types as `.?AV<name>@<scope>@@`.
        // The engine consistently namespaces gameplay components in `pa::`, so this name is stable across patches that
        // do not rename or move the class. The match is an exact byte-equal compare with no substring scan, which
        // rules out a collision with a derived or sibling class that shares a prefix.
        constexpr std::string_view CCC_TYPE_DESCRIPTOR_NAME = ".?AVClientCharacterControlActorComponent@pa@@";

        // Maximum number of component-pointer slots to scan inside the p1 table. The table is component-type-id indexed
        // and therefore sparse. A typical protagonist populates roughly 30 slots inside a 64-slot window. A 64-slot
        // sweep covers any realistic engine expansion and keeps the cold-path RTTI scan bounded to a single page read.
        constexpr std::size_t COMPONENT_TABLE_SLOTS = 64;

        // MSVC std::string: when content >= 16 chars the heap-buffer pointer lives at +0x00 of the struct. A smaller
        // string keeps inline content at the same offset. All known protagonist appearance paths are 70+ chars (always
        // heap-allocated) but the resolver falls back to an inline read on the off-chance the engine produces a
        // short variant in a future update.
        constexpr std::ptrdiff_t OFF_STRING_BUF_PTR = 0x00;
        constexpr std::size_t APPEAR_PATH_READ_MAX = 160;

        // Anchor: marks the start of the character subfolder name in `.../cd_<archetype>_<codename>/...`. Codename
        // search is restricted to the suffix after this anchor so a path component earlier in the tree that happens to
        // contain a codename substring (very unlikely, but defensive) cannot cause a false positive.
        constexpr std::string_view APPEAR_ANCHOR = "/cd_";

        // Default character-subfolder substrings. Each protagonist's appearance path embeds the full subfolder name
        // twice (subfolder + filename), so a plain substring search is reliable. The default is the full
        // `cd_<archetype>_<codename>` form rather than the bare codename because:
        //   - it is self-documenting (a user who reads the INI sees the asset-path shape directly),
        //   - it avoids any chance of a coincidental match against an unrelated path component that happens to contain
        //     a short codename like "damian",
        //   - the substring is still short enough that a user-side override can shorten it for a wider match.
        // The tokens are mutable at runtime via set_protagonist_codenames() so a mod or game patch that renames a
        // subfolder can be patched without a recompile. s_codename_mutex guards them. The read path snapshots all three
        // under one lock and releases it before the search.
        constexpr std::string_view DEFAULT_CODENAME_KLIFF = "cd_phm_macduff";
        constexpr std::string_view DEFAULT_CODENAME_DAMIANE = "cd_phw_damian";
        constexpr std::string_view DEFAULT_CODENAME_OONGKA = "cd_phm_oongka";

        // Mutable internal state

        // Track Kliff CCOIA pointer to drive world_generation() bumps and kliff_low cache invalidation. The sub-manager
        // pointer is persistent across save-load and cannot be used here. Kliff's CCOIA IS reallocated on every
        // save-load, and its session-local low byte at +0x60 shifts between sessions (e.g., 0x01 in one session, 0x05
        // in another), so the Kliff CCOIA pointer is the correct rebuild signal.
        std::atomic<std::uintptr_t> s_cached_kliff_ccoia{0};
        std::atomic<std::uint64_t> s_world_generation{0};

        // Codename storage. Initialized to the engine defaults. set_protagonist_codenames() mutates it at
        // config-load time and on auto-reload.
        std::mutex s_codename_mutex;
        std::string s_codename_kliff{DEFAULT_CODENAME_KLIFF};
        std::string s_codename_damiane{DEFAULT_CODENAME_DAMIANE};
        std::string s_codename_oongka{DEFAULT_CODENAME_OONGKA};

        // Player base resolution

        std::uintptr_t resolve_player_base_address() noexcept
        {
            // Lazy one-shot AOB resolve. The engine publishes the pa::ClientActorManager* into a single static slot
            // whose module-relative offset drifts between game patches. The cascade in anchors.hpp anchors on three
            // distinct instructions inside the publishing function, so a partial recompile or a compiler reorder still
            // resolves.
            static std::atomic<std::uintptr_t> s_cached{0};
            const auto cached = s_cached.load(std::memory_order_acquire);
            if (DMK::memory::is_plausible_ptr(DMK::Address{cached}))
                return cached;

            // The scan scope is the host image: faster than a whole-process walk, and immune to a generic-shaped
            // candidate first-matching inside a sibling mod or an overlay. Pages::Executable narrows the byte sweep to
            // code pages, because every rung anchors on an instruction inside the publishing function and an identical
            // run in .rdata is an alias, never the intended site.
            const auto hit = DMK::scan::resolve(
                DMK::scan::ScanRequest{
                    .ladder = anchors::CLIENT_ACTOR_MANAGER_GLOBAL_CANDIDATES,
                    .label = "ClientActorManagerGlobal",
                    .scope = DMK::Region::host(),
                    .pages = DMK::scan::Pages::Executable,
                }
            );
            if (!hit)
            {
                DMK::log().warning("ClientActorManagerGlobal did not resolve: {}", hit.error().message());
                return 0;
            }

            const auto addr = hit->address.raw();
            if (!DMK::memory::is_plausible_ptr(DMK::Address{addr}))
            {
                DMK::log().warning(
                    "ClientActorManagerGlobal resolved to an implausible slot address {:#x}: the chain stays "
                    "unresolved",
                    addr
                );
                return 0;
            }
            // Every walk reads the manager through this slot, so the publish must release.
            s_cached.store(addr, std::memory_order_release);
            return addr;
        }

        // Chain walks
        //
        // walk_chain_seh bundles every dereference along its path under one SEH frame because it heals the manager
        // offset mid-chain and forks into two leaves. One frame also keeps the "any fault aborts the walk" property
        // that mid-teardown windows rely on: a torn intermediate pointer short-circuits the whole walk, not one leaf.
        //
        // A linear chain with no mid-walk side effect goes through `DMK::memory::walk` instead, and a single-deref
        // read uses `DMK::memory::read<T>(DMK::Address{addr}).value_or(0)` inline at the call site.

        struct ChainAnchors
        {
            /// pa::ClientActorManager.
            std::uintptr_t mgr = 0;
            /// pa::ClientUserActor.
            std::uintptr_t user_actor = 0;
            /// CCOIA sub-manager.
            std::uintptr_t sub_mgr = 0;
            /// Kliff CCOIA, at sub-manager +0x30.
            std::uintptr_t kliff_ccoia = 0;
            /// Controlled CCOIA, at sub-manager +0x38.
            std::uintptr_t controlled = 0;
        };

        ChainAnchors walk_chain_seh() noexcept
        {
            ChainAnchors out{};
            const auto player_base = resolve_player_base_address();
            if (!DMK::memory::is_plausible_ptr(DMK::Address{player_base}))
                return out;
            __try
            {
                const auto mgr = *reinterpret_cast<const volatile std::uintptr_t *>(player_base);
                if (!DMK::memory::is_plausible_ptr(DMK::Address{mgr}))
                    return out;
                check_manager_type(mgr);
                heal_chain_offsets(mgr);
                const auto user_actor = *reinterpret_cast<const volatile std::uintptr_t *>(mgr + off_user_actor());
                if (!DMK::memory::is_plausible_ptr(DMK::Address{user_actor}))
                    return out;
                const auto sub_mgr = *reinterpret_cast<const volatile std::uintptr_t *>(user_actor + OFF_SUB_MANAGER);
                if (!DMK::memory::is_plausible_ptr(DMK::Address{sub_mgr}))
                    return out;
                out.mgr = mgr;
                out.user_actor = user_actor;
                out.sub_mgr = sub_mgr;
                out.kliff_ccoia = *reinterpret_cast<const volatile std::uintptr_t *>(sub_mgr + OFF_SUB_MGR_KLIFF);
                out.controlled = *reinterpret_cast<const volatile std::uintptr_t *>(sub_mgr + OFF_SUB_MGR_CTRL);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // Partial state may be set. The caller checks each anchor for non-zero before use.
            }
            return out;
        }

        // Guarded resolution of the 100-entry actor list base from a ClientUserActor. Walks user_actor+0x78
        // (vec_data) -> +0x20 (vec[2] = ChildContainer ptr) -> +0x18 (actor list ptr). The trace publishes the two
        // intermediate anchors through @p out_vec and @p out_child so the diagnostic caller can report them. Returns 0
        // when any link is null or torn, otherwise the actor-list base.
        std::uintptr_t
        walk_to_actor_list_seh(std::uintptr_t user_actor, std::uintptr_t &out_vec, std::uintptr_t &out_child) noexcept
        {
            constexpr std::array<std::ptrdiff_t, 4> steps{OFF_USER_VEC, OFF_VEC_CHILD_SLOT, OFF_CHILD_LIST, 0};
            std::array<DMK::Address, 4> trace{};
            const auto list = DMK::memory::walk(DMK::Address{user_actor}, steps, trace);
            out_vec = trace[0].raw();
            out_child = trace[1].raw();

            // The walk records a hop only after the link clears the plausibility screen, so a torn link leaves
            // its trace slot at 0 and the diagnostic loses the very value that names the break. Re-read the two
            // intermediate links directly in that case. Both reads are guarded, run only on the failure path of a
            // diagnostic-only walker, and report the raw bytes the chain published.
            if (out_vec == 0)
                out_vec = DMK::memory::read<std::uintptr_t>(DMK::Address{user_actor + OFF_USER_VEC}).value_or(0);
            if (out_child == 0 && out_vec != 0)
                out_child = DMK::memory::read<std::uintptr_t>(DMK::Address{out_vec + OFF_VEC_CHILD_SLOT}).value_or(0);

            return list ? list->raw() : 0;
        }

        // World-generation tracking

        // Update world-generation counter when Kliff CCOIA identity changes (= save-load: the engine reallocates
        // Kliff's CCOIA with a fresh session-local +0x60 lo byte). Flushes the kliff_low cache so the delta classifier
        // reads the new anchor rather than a stale value.
        void note_chain_observation(std::uintptr_t kliff_ccoia) noexcept
        {
            if (!DMK::memory::is_plausible_ptr(DMK::Address{kliff_ccoia}))
                return;
            const auto last = s_cached_kliff_ccoia.load(std::memory_order_acquire);
            if (kliff_ccoia == last)
                return;
            std::uintptr_t expected = last;
            // Do NOT weaken these to relaxed. No consumer reads state published behind the counter today, so relaxed
            // would be correct for the payload alone, but two edges depend on the stronger order. The acquire halves
            // let this observer see the release store invalidate_controlled_character() uses to force a refresh, so a
            // weaker load defers that refresh by a poll. The release half keeps the pair with the acquire load in
            // world_generation() real, so a consumer that later puts state behind the bump does not inherit a silent
            // race. Every order here lowers to a plain mov on x86-64, so the strength costs nothing.
            if (s_cached_kliff_ccoia.compare_exchange_strong(
                    expected,
                    kliff_ccoia,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                ))
            {
                s_world_generation.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        // ASCII reader for std::string content

        // Read up to (cap - 1) printable ASCII bytes from @p start into @p buf, up to the first NUL. Returns false
        // on a torn read or on the first non-printable byte, which rejects garbage early so the classifier does not
        // pattern-match on partial pointer bytes. The buffer is NUL-terminated on success.
        bool safe_read_ascii(std::uintptr_t start, char *buf, std::size_t cap, std::size_t &out_len) noexcept
        {
            out_len = 0;
            if (!DMK::memory::is_plausible_ptr(DMK::Address{start}) || buf == nullptr || cap == 0)
                return false;
            __try
            {
                std::size_t i = 0;
                const auto limit = cap - 1;
                while (i < limit)
                {
                    const auto b = *reinterpret_cast<const volatile std::uint8_t *>(start + i);
                    if (b == 0)
                        break;
                    if (b < 0x20 || b > 0x7E)
                        return false;
                    buf[i] = static_cast<char>(b);
                    ++i;
                }
                buf[i] = '\0';
                out_len = i;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // RTTI-based component-table walker

        // Cached vtable address of the target component class (.?AVClientCharacterControlActorComponent@pa@@).
        // Image-resident and stable for the process lifetime. An RTTI scan learns it once, on the first successful
        // chain walk.
        DMK::rtti::PointerTableCache s_ccc_vtable;

        // Walk CCOIA +0x68 -> [CCC via RTTI] -> +0x40 -> +0x38 to reach the appearance-config std::string. Resolves
        // heap-buffer against inline layout and returns the start address of ASCII content. Returns 0 on any torn
        // link.
        std::uintptr_t resolve_appearance_path_buffer(std::uintptr_t ccoia) noexcept
        {
            if (!DMK::memory::is_plausible_ptr(DMK::Address{ccoia}))
                return 0;
            const auto p1 = DMK::memory::read<std::uintptr_t>(DMK::Address{ccoia + OFF_APPEAR_CHAIN1}).value_or(0);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{p1}))
                return 0;
            const auto ccc_hit = DMK::rtti::find_in_pointer_table(
                DMK::Address{p1},
                COMPONENT_TABLE_SLOTS,
                CCC_TYPE_DESCRIPTOR_NAME,
                s_ccc_vtable
            );
            const std::uintptr_t ccc = ccc_hit ? ccc_hit->raw() : 0;
            if (!DMK::memory::is_plausible_ptr(DMK::Address{ccc}))
                return 0;
            const auto p3 = DMK::memory::read<std::uintptr_t>(DMK::Address{ccc + OFF_APPEAR_CHAIN3}).value_or(0);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{p3}))
                return 0;
            const auto str_struct = DMK::memory::read<std::uintptr_t>(DMK::Address{p3 + OFF_APPEAR_CHAIN4}).value_or(0);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{str_struct}))
                return 0;
            const auto buf_ptr =
                DMK::memory::read<std::uintptr_t>(DMK::Address{str_struct + OFF_STRING_BUF_PTR}).value_or(0);
            if (DMK::memory::is_plausible_ptr(DMK::Address{buf_ptr}))
                return buf_ptr;
            return str_struct; // inline fallback
        }

        // CCOIA classification

        // Classifies a CCOIA: it reads the appearance-config path and matches the embedded character codename.
        // Returns 1/2/3 for Kliff/Damiane/Oongka, or 0 when the chain is unreachable or the path carries no known
        // codename (NPCs and follower humanoids fall here - they either lack an appearance config at this offset or
        // carry an unknown codename).
        //
        // The classifier anchors on the "/cd_" substring (start of the character subfolder name in
        // `.../cd_<archetype>_<codename>/...`) and searches for the codename inside the remaining suffix. That avoids
        // a false positive when a codename's bytes appear earlier in the path tree (e.g., a directory name that
        // coincides with a substring of a character codename).
        std::uint32_t classify_by_appearance(std::uintptr_t ccoia) noexcept
        {
            const auto str_start = resolve_appearance_path_buffer(ccoia);
            if (str_start == 0)
                return 0;
            char buf[APPEAR_PATH_READ_MAX]{};
            std::size_t len = 0;
            if (!safe_read_ascii(str_start, buf, sizeof(buf), len))
                return 0;
            return classify_appearance_by_path(std::string_view{buf, len});
        }

        // Primary CCOIA classifier. Tries the appearance-config path first (character-codename identity that survives
        // outfit and state changes). Falls back to the +0x63 high-byte fast-path for Kliff only when the appearance
        // chain is mid-teardown (engine save-load window where component pointers transiently null). Damiane and Oongka
        // are not distinguishable without the appearance chain, so the resolver returns Unknown for them in that
        // window rather than a guess.
        std::uint32_t classify_ccoia(std::uintptr_t ccoia) noexcept
        {
            if (!DMK::memory::is_plausible_ptr(DMK::Address{ccoia}))
                return 0;
            const auto by_appearance = classify_by_appearance(ccoia);
            if (by_appearance != 0)
                return by_appearance;
            const auto packed = DMK::memory::read<std::uint32_t>(DMK::Address{ccoia + OFF_CCOIA_IDENTITY}).value_or(0);
            const auto high_byte = static_cast<std::uint8_t>((packed >> 24) & 0xFFu);
            if (high_byte == KLIFF_HIGH_BYTE)
                return 1;
            return 0;
        }

        /// Maps a controlled character onto the 1-based index the public API publishes.
        std::uint32_t character_index(ControlledCharacter ch) noexcept
        {
            switch (ch)
            {
            case ControlledCharacter::Kliff:
                return 1;
            case ControlledCharacter::Damiane:
                return 2;
            case ControlledCharacter::Oongka:
                return 3;
            default:
                return 0;
            }
        }

    } // namespace

    // Public API

    std::uintptr_t current_controlled_ccoia() noexcept
    {
        const auto chain = walk_chain_seh();
        note_chain_observation(chain.kliff_ccoia);
        return chain.controlled;
    }

    ControlledCharacter current_controlled_character() noexcept
    {
        const auto chain = walk_chain_seh();
        note_chain_observation(chain.kliff_ccoia);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{chain.controlled}))
            return ControlledCharacter::Unknown;
        const auto idx = classify_ccoia(chain.controlled);
        switch (idx)
        {
        case 1:
            return ControlledCharacter::Kliff;
        case 2:
            return ControlledCharacter::Damiane;
        case 3:
            return ControlledCharacter::Oongka;
        default:
            return ControlledCharacter::Unknown;
        }
    }

    std::string_view controlled_character_name(ControlledCharacter ch) noexcept
    {
        switch (ch)
        {
        case ControlledCharacter::Kliff:
            return "Kliff";
        case ControlledCharacter::Damiane:
            return "Damiane";
        case ControlledCharacter::Oongka:
            return "Oongka";
        default:
            return {};
        }
    }

    ControlledCharacter character_from_name(std::string_view name) noexcept
    {
        if (name == "Kliff")
            return ControlledCharacter::Kliff;
        if (name == "Damiane")
            return ControlledCharacter::Damiane;
        if (name == "Oongka")
            return ControlledCharacter::Oongka;
        return ControlledCharacter::Unknown;
    }

    std::uint32_t character_idx_from_name(std::string_view name) noexcept
    {
        return character_index(character_from_name(name));
    }

    std::string_view current_controlled_character_name() noexcept
    {
        return controlled_character_name(current_controlled_character());
    }

    std::uint32_t current_controlled_character_idx() noexcept
    {
        return character_index(current_controlled_character());
    }

    std::uint32_t character_idx_for_ccoia(std::uintptr_t ccoia) noexcept
    {
        return classify_ccoia(ccoia);
    }

    std::uintptr_t equip_slot_for_ccoia(std::uintptr_t ccoia) noexcept
    {
        constexpr std::array<std::ptrdiff_t, 3> steps{OFF_APPEAR_CHAIN1, OFF_EQUIP_SLOT_COMPONENT, 0};
        const auto slot = DMK::memory::walk(DMK::Address{ccoia}, steps);
        return slot ? slot->raw() : 0;
    }

    std::uint32_t classify_appearance_by_path(std::string_view path) noexcept
    {
        if (path.empty())
            return 0;
        const auto anchor = path.find(APPEAR_ANCHOR);
        if (anchor == std::string_view::npos)
            return 0;
        const auto suffix = path.substr(anchor);

        // Snapshot codenames under one lock, then release it before the substring search. Empty codenames are
        // treated as "skip this protagonist" rather than "match everything" (find("") returns 0 = always-hit).
        std::string kliff, damiane, oongka;
        {
            std::lock_guard<std::mutex> lock(s_codename_mutex);
            kliff = s_codename_kliff;
            damiane = s_codename_damiane;
            oongka = s_codename_oongka;
        }
        if (!kliff.empty() && suffix.find(kliff) != std::string_view::npos)
            return 1;
        if (!damiane.empty() && suffix.find(damiane) != std::string_view::npos)
            return 2;
        if (!oongka.empty() && suffix.find(oongka) != std::string_view::npos)
            return 3;
        return 0;
    }

    std::size_t snapshot_body_cache(BodyCacheEntry *out, std::size_t cap) noexcept
    {
        if (out == nullptr || cap == 0)
            return 0;
        const auto chain = walk_chain_seh();
        note_chain_observation(chain.kliff_ccoia);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{chain.kliff_ccoia}))
            return 0;

        std::size_t written = 0;
        // Kliff is always the first entry (always present).
        out[written++] = {chain.kliff_ccoia, 1u};

        if (written >= cap)
            return written;

        // Resolve the actor-array descriptor against the live manager before the read. The walked chain supplies the
        // Kliff CCOIA the heal needs as its discriminator, and the call latches after the first success.
        heal_mgr_actor_array(chain.mgr, chain.kliff_ccoia);

        // Walk the actor array. For each non-Kliff entry, run the appearance-config classifier. NPCs and followers
        // fail the codename-substring match (their appearance path does not contain `cd_phw_damian` or
        // `cd_phm_oongka`), so the loop emits at most one Damiane and one Oongka entry.
        //
        // The loop scans the array's full live extent rather than a fixed prefix: the bound comes from the engine's
        // clamped capacity field, so a crowded scene that pushes a protagonist hundreds of entries deep still
        // resolves.
        // Null/torn slots are skipped (continue), never treated as end-of-array, so an interior hole or a transient
        // unreadable slot cannot truncate the scan and drop a protagonist behind it. The early-out below halts
        // the walk as soon as both companions are found, so the full sweep only runs when one is genuinely absent.
        const auto actor_array =
            DMK::memory::read<std::uintptr_t>(DMK::Address{chain.mgr + off_mgr_actor_array()}).value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{actor_array}))
            return written;

        const auto raw_cap =
            DMK::memory::read<std::uint32_t>(DMK::Address{chain.mgr + off_mgr_actor_array_cap()}).value_or(0);
        const std::size_t scan_cap = (raw_cap < MGR_ARRAY_CAP_MIN) ? MGR_ARRAY_SCAN_FALLBACK
                                     : (raw_cap > MGR_ARRAY_CAP_HARD_CAP)
                                         ? static_cast<std::size_t>(MGR_ARRAY_CAP_HARD_CAP)
                                         : static_cast<std::size_t>(raw_cap);

        bool found_damiane = false;
        bool found_oongka = false;
        for (std::size_t i = 0; i < scan_cap && written < cap; ++i)
        {
            const auto candidate =
                DMK::memory::read<std::uintptr_t>(DMK::Address{actor_array + i * sizeof(std::uintptr_t)}).value_or(0);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{candidate}) || candidate == chain.kliff_ccoia)
                continue;
            const auto idx = classify_ccoia(candidate);
            if (idx == 2 && !found_damiane)
            {
                out[written++] = {candidate, 2u};
                found_damiane = true;
            }
            else if (idx == 3 && !found_oongka)
            {
                out[written++] = {candidate, 3u};
                found_oongka = true;
            }
            if (found_damiane && found_oongka)
                break;
        }

        return written;
    }

    ActorListDebugSummary debug_enumerate_actor_list(ActorListDebugEntry *out, std::size_t cap) noexcept
    {
        ActorListDebugSummary summary{};
        if (out == nullptr || cap == 0)
            return summary;

        const auto chain = walk_chain_seh();
        note_chain_observation(chain.kliff_ccoia);
        summary.mgr = chain.mgr;
        summary.user_actor = chain.user_actor;
        summary.sub_mgr = chain.sub_mgr;
        summary.kliff_ccoia = chain.kliff_ccoia;
        summary.controlled = chain.controlled;

        // Reuse the shared chain-to-list walker. The diagnostic summary publishes the intermediate anchors the
        // walker already collects.
        const auto actor_list = walk_to_actor_list_seh(chain.user_actor, summary.vec_data, summary.child_container);
        summary.actor_list = actor_list;
        if (!DMK::memory::is_plausible_ptr(DMK::Address{actor_list}))
            return summary;

        __try
        {
            std::size_t n = 0;
            for (std::size_t i = 0; i < ACTOR_LIST_CAPACITY && n < cap; ++i)
            {
                const auto entry_base = actor_list + i * ACTOR_LIST_STRIDE;
                const auto ccoia = *reinterpret_cast<const volatile std::uintptr_t *>(entry_base);
                const auto flag = *reinterpret_cast<const volatile std::uint64_t *>(entry_base + 8);
                if (!DMK::memory::is_plausible_ptr(DMK::Address{ccoia}) && flag == 0)
                    continue;
                std::uint32_t ident = 0;
                if (DMK::memory::is_plausible_ptr(DMK::Address{ccoia}))
                    ident = DMK::memory::read<std::uint32_t>(DMK::Address{ccoia + OFF_CCOIA_IDENTITY}).value_or(0);
                out[n].ccoia = ccoia;
                out[n].flag = flag;
                out[n].identity = ident;
                ++n;
            }
            summary.raw_entries = n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Partial state. Return the entries collected before the fault.
        }
        return summary;
    }

    std::uint64_t world_generation() noexcept
    {
        // Take an opportunistic chain snapshot so the counter refreshes even for a caller that never queries
        // identity. The counter itself is monotonic and never regresses on a transient null window.
        const auto chain = walk_chain_seh();
        note_chain_observation(chain.kliff_ccoia);
        return s_world_generation.load(std::memory_order_acquire);
    }

    void invalidate_controlled_character() noexcept
    {
        s_cached_kliff_ccoia.store(0, std::memory_order_release);
    }

    void set_protagonist_codenames(std::string_view kliff, std::string_view damiane, std::string_view oongka) noexcept
    {
        std::lock_guard<std::mutex> lock(s_codename_mutex);
        if (!kliff.empty())
            s_codename_kliff = kliff;
        if (!damiane.empty())
            s_codename_damiane = damiane;
        if (!oongka.empty())
            s_codename_oongka = oongka;
    }

    std::uintptr_t find_component_in_table(
        std::uintptr_t p1,
        std::string_view rtti_name,
        DMK::rtti::PointerTableCache &vtable_cache
    ) noexcept
    {
        const auto hit =
            DMK::rtti::find_in_pointer_table(DMK::Address{p1}, COMPONENT_TABLE_SLOTS, rtti_name, vtable_cache);
        return hit ? hit->raw() : 0;
    }

    std::uintptr_t find_component_for_equipslot(
        std::uintptr_t equip_slot,
        std::string_view rtti_name,
        DMK::rtti::PointerTableCache &vtable_cache
    ) noexcept
    {
        if (!DMK::memory::is_plausible_ptr(DMK::Address{equip_slot}))
            return 0;
        // ClientEquipSlotActorComponent + 0x08 = back-pointer to pa::ClientChildOnlyInGameActor (the CCOIA). Then the
        // standard CCOIA + OFF_APPEAR_CHAIN1 hop to the component table.
        constexpr std::ptrdiff_t OFF_EQUIP_SLOT_CCOIA_BACKREF = 0x08;
        const auto ccoia =
            DMK::memory::read<std::uintptr_t>(DMK::Address{equip_slot + OFF_EQUIP_SLOT_CCOIA_BACKREF}).value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{ccoia}))
            return 0;
        const auto p1 = DMK::memory::read<std::uintptr_t>(DMK::Address{ccoia + OFF_APPEAR_CHAIN1}).value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{p1}))
            return 0;
        return find_component_in_table(p1, rtti_name, vtable_cache);
    }

} // namespace CDCore
