#pragma once

// Per-slot swatch table.
//
// Each row corresponds to ONE shader-property write the engine emits
// during a captured apply for an LT slot. A row's identity tuple is:
//
//   (content_hash, stable_id, template_id, token_id)
//
//   - content_hash = u32 at arec+0x40 (publisher writes the arec into
//     matInst+0xA0; the hash is stable across pool recycling).
//   - stable_id    = u64 at matInst+0x80 (per-matInst pool counter).
//   - template_id  = u16 at matInst+0x48 (shader template variant).
//   - token_id     = u16 from a2-8+0x28 (shader-property token).
//
// SwatchEntry holds the engine-default RGBA captured at row first
// fire (when override_active == false). SwatchOverride holds the
// UI-mutable state (user RGB, per-row override_active flag) and is
// a parallel array indexed by row index. The two arrays share a
// single row index per slot.

#include "color_override.hpp"   // for k_dyeSwatchesPerSlot
#include "shared_state.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Transmog::ColorOverride::SwatchTable
{
    using ::Transmog::k_slotCount;
    using ::Transmog::ColorOverride::k_dyeSwatchesPerSlot;

    /// Engine-observed row identity + cached engine-default RGBA.
    /// Setter-thread writes; UI thread reads (atomic loads only).
    struct SwatchEntry
    {
        std::atomic<std::uint32_t> content_hash{0};
        std::atomic<std::uint64_t> stable_id{0};
        std::atomic<std::uint16_t> template_id{0};
        std::atomic<std::uint16_t> token_id{0};
        std::atomic<bool>          active_this_apply{false};

        // Engine default 4-byte property value at row first fire when
        // override_active was false. Stored canonically as R,G,B,A
        // (the setter writes BGRA so we unpack on capture).
        std::atomic<std::uint8_t>  def_r{0};
        std::atomic<std::uint8_t>  def_g{0};
        std::atomic<std::uint8_t>  def_b{0};
        std::atomic<std::uint8_t>  def_a{0xFF};
        std::atomic<std::uint8_t>  def_seen_mask{0}; // bit0=RGB, bit1=A
        std::atomic<bool>          def_a_captured{false};

        // Reinit-pruned ghost row. Set true by Reinit::Finalize on
        // identities that didn't appear in all 3 capture passes.
        // While true, the setter's substitute path bails on it.
        std::atomic<bool>          frozen_hidden{false};
    };

    /// UI-mutable per-row override state. Parallel array shares the
    /// row index with SwatchEntry. UI thread writes; setter thread
    /// reads (atomic loads only).
    ///
    /// Each entry carries the per-row identity (token_id /
    /// submesh_stable_id / template_id) plus the captured alpha so
    /// the picker can label rows and group by region without a
    /// cross-array lookup.
    ///
    /// Layout is plain POD (no atomics) so picker code can read /
    /// write fields directly. The cross-thread races between the
    /// game-thread setter and the UI thread are bounded:
    ///   * single-byte fields (r, g, b, override_active flags) --
    ///     hardware-atomic on x86, no tearing risk;
    ///   * multi-byte fields (token_id u16, submesh_stable_id u64,
    ///     template_id u16) -- written once at row insertion and
    ///     never mutated after, so reads cannot tear.
    struct SwatchOverride
    {
        bool          override_active = false;
        std::uint8_t  r = 0;
        std::uint8_t  g = 0;
        std::uint8_t  b = 0;
        std::uint8_t  def_r = 0;
        std::uint8_t  def_g = 0;
        std::uint8_t  def_b = 0;
        std::uint8_t  def_a = 0xFF;
        bool          default_captured = false;
        bool          def_a_captured = false;
        std::uint16_t token_id = 0;
        std::uint64_t submesh_stable_id = 0;
        std::uint16_t template_id = 0;
        // Captured from `SkinnedMeshMaterialWrapper +0x28 -> +0x18`
        // at publisher-hook time. ASCIIZ in the format
        // `cd_phm_00_<region>_NNNN[_NN_NN]`. Empty when capture
        // failed (cold pool / non-pac source / SEH bail). 40 chars
        // covers the engine's inline string-wrapper capacity (32
        // bytes) with safety margin.
        char          submesh_name[40] = {0};
    };

    using DyeRGB = SwatchOverride;

    /// Lookup-or-insert keyed by full identity tuple. Returns row
    /// index in [0..k_dyeSwatchesPerSlot) on success, -1 if the slot
    /// is full or frozen or any required field is 0. `expect_open`
    /// gates inserts (false means lookup-only).
    int lookup_or_insert(int slot,
                         std::uint32_t content_hash,
                         std::uint64_t stable_id,
                         std::uint16_t template_id,
                         std::uint16_t token_id,
                         bool expect_open) noexcept;

    /// Overload that also seeds the per-row submesh name (captured
    /// from `SkinnedMeshMaterialWrapper +0x28` at publisher-hook
    /// time). Name is only WRITTEN on insert; on lookup-hit the
    /// existing row's name is preserved. Pass nullptr or empty to
    /// behave identically to the non-name overload.
    int lookup_or_insert(int slot,
                         std::uint32_t content_hash,
                         std::uint64_t stable_id,
                         std::uint16_t template_id,
                         std::uint16_t token_id,
                         bool expect_open,
                         const char *submesh_name) noexcept;

    /// Count of rows currently used in the slot's table.
    std::size_t count(int slot) noexcept;

    /// Read row by index. Returns nullptr if out of range.
    SwatchEntry    *row(int slot, std::size_t idx) noexcept;
    SwatchOverride *override_row(int slot, std::size_t idx) noexcept;

    /// Setter-side: capture the engine-default RGBA into the row's
    /// SwatchEntry AND mirror to SwatchOverride for the UI. No-op if
    /// the row's override is currently active OR a default was
    /// already captured. `r,g,b,a` are the canonical color channels
    /// (caller converts from shader-side byte order).
    void capture_default_if_unset(int slot, std::size_t idx,
                                  std::uint8_t r, std::uint8_t g,
                                  std::uint8_t b, std::uint8_t a) noexcept;

    /// UI-side helpers --------------------------------------------------

    /// Set a per-row override RGB. Setter reads `r/g/b` when
    /// `override_active` is true.
    void set_override_rgb(int slot, std::size_t idx,
                          std::uint8_t r, std::uint8_t g,
                          std::uint8_t b) noexcept;

    /// Toggle a per-row override. While off, the setter falls
    /// through and the engine's natural value flows.
    void set_override_active(int slot, std::size_t idx,
                             bool active) noexcept;

    /// Quick check: is any row's override_active flag set for the
    /// given slot? The setter uses this together with the per-slot
    /// master-enable gate (`slot_enabled_get`) to bail before
    /// reading the full row.
    bool any_override_active_in_slot(int slot) noexcept;

    /// Find the slot whose user-saved placeholder matches the given
    /// (submesh_name, token_id), or -1 if none exists.
    ///
    /// Placeholder rows are created by `populate_from_persisted`
    /// from JSON: `content_hash == 0` + non-empty `submesh_name` +
    /// non-zero `token_id` + `override_active = true`. They are the
    /// authoritative "user wants this slot's color on this submesh"
    /// record.
    ///
    /// The publisher hook attributes matInsts to whichever slot's
    /// apply window is active when the engine re-binds them. The
    /// Chest carrier-hybrid byte-patch causes the engine to rebuild
    /// the entire character body mesh, so helm / boots / glove
    /// matInsts get re-bound DURING the Chest apply window and end
    /// up in `carrier_set[Chest]`. `resolve_slot()` would then
    /// return Chest for helm dye writes; the substitute path uses
    /// this helper to re-route to the slot the user actually saved
    /// the placeholder in.
    ///
    /// Empty submesh_name bails, token_id 0 bails. Lowest-index
    /// slot wins on duplicates (deterministic). Linear scan,
    /// `k_slotCount * 256` rows worst case -- cheap enough for the
    /// hot path.
    int find_placeholder_slot(const char *submesh_name,
                              std::uint16_t token_id) noexcept;

    /// Promote a placeholder row to a bound row by writing the live
    /// matInst's identity fields into it. Companion to
    /// `find_placeholder_slot`: same search (placeholders with
    /// `content_hash == 0` matching `submesh_name + token_id`), but
    /// on hit it stores `template_id` / `stable_id` / `content_hash`
    /// into the row's `SwatchEntry` atomics so the picker UI shows
    /// real `tpl 0x%04X` values instead of `tpl 0x0000`.
    ///
    /// Best-effort: returns true if a placeholder was promoted,
    /// false if no matching placeholder exists (already promoted,
    /// never seeded, or arguments invalid). The slot-agnostic
    /// pending-match path in the setter substitute calls this so
    /// its hits enrich the row's metadata as a side effect, even
    /// though the actual substitution doesn't consult the row.
    ///
    /// Safe to call concurrently from multiple setter threads --
    /// all writes are atomic, and last-writer-wins is fine because
    /// every caller writes the same matInst's identity.
    bool promote_placeholder_identity(
        const char *submesh_name,
        std::uint16_t token_id,
        std::uint32_t content_hash,
        std::uint64_t stable_id,
        std::uint16_t template_id,
        std::uint8_t def_r,
        std::uint8_t def_g,
        std::uint8_t def_b,
        std::uint8_t def_a) noexcept;

    void mark_all_inactive(int slot) noexcept;
    void clear_slot(int slot) noexcept;
    void clear_all() noexcept;

    // ---- Per-slot DyeSlot view ---------------------------------------
    //
    // UI-facing view over the per-slot swatch storage.
    // `slot_enabled` is the per-slot override master switch. The
    // picker reads the per-row SwatchOverride storage directly via
    // `dye_state()[slot].swatches[idx]`.

    /// Per-slot master-enable gate. The UI binds an ImGui::Checkbox
    /// to the returned reference.
    bool slot_enabled_get(int slot) noexcept;
    void slot_enabled_set(int slot, bool v) noexcept;

    /// Detected swatch count for the slot (the in-use row count).
    /// The picker gates the master Dye checkbox and the "show
    /// swatch UI" branch on this being > 0.
    std::size_t detected_swatch_count(int slot) noexcept;

    /// Wipe the slot's apply-window timers and carrier-set state
    /// without touching the swatch rows or user override choices.
    /// The picker invokes this after the Reset Slot button so the
    /// next apply re-captures cleanly.
    void clear_dye_state_for_slot(int slot) noexcept;

    // ---- Reinit-aware accessors -------------------------------------

    /// Wipe ALL rows + override choices for slot. Use when the
    /// transmog target item changes so stale identities don't bleed
    /// into the new item's capture set. Also clears the reinit lock
    /// + capture-window state.
    void wipe_swatch_table_for_slot(int slot) noexcept;

    /// True if the slot was wiped by `wipe_swatch_table_for_slot`
    /// (user-triggered Reset Slot) and has not been repopulated
    /// since. Read by PresetManager::snapshot_live_swatches_into to
    /// distinguish a user-intentional empty state from the
    /// token-resolution race where live can be empty but the JSON
    /// baseline must be preserved.
    bool slot_was_explicitly_wiped(int slot) noexcept;

    /// Clear the explicit-wipe flag after the empty state has been
    /// persisted into the active preset. Called by the snapshot
    /// path so subsequent saves on the same slot don't keep
    /// re-flagging an already-empty preset.
    void clear_explicit_wipe_flag(int slot) noexcept;

    // ---- Reinit gates (owned by SwatchTable, read by setter) --------

    /// Post-reinit lock: once Finalize completes, the slot's identity
    /// set is closed. The setter's lookup_or_insert refuses to add
    /// new rows but still hits existing ones for substitution.
    std::atomic<bool> &post_reinit_lock(int slot) noexcept;

    /// Reinit capture window: open during start..Finalize, closed at
    /// Finalize. While the post-reinit lock is true, this also gates
    /// any further inserts.
    std::atomic<bool> &reinit_capture_open(int slot) noexcept;

    /// Convenience: identity tuple snapshot for currently-active rows.
    /// Used by the reinit state machine to capture per-pass keys.
    struct SwatchIdentity
    {
        std::uint32_t hash;
        std::uint64_t stable;
        std::uint16_t tpl;
        std::uint16_t token;
        bool operator==(const SwatchIdentity &o) const noexcept
        {
            return hash == o.hash && stable == o.stable
                && tpl == o.tpl && token == o.token;
        }
    };
    /// Snapshot identity tuples for rows with active_this_apply set.
    std::size_t snapshot_active_identities(int slot,
                                           SwatchIdentity *out,
                                           std::size_t out_cap) noexcept;

    /// Apply the keep-set computed by Reinit::Finalize. Rows whose
    /// identity matches an entry in `keep` get cleared of
    /// `frozen_hidden` + `active_this_apply=true`; rows that don't
    /// match get `frozen_hidden=true` + `active_this_apply=false`.
    /// Returns (kept_count, hidden_count).
    struct KeepResult { std::size_t kept = 0, hidden = 0; };
    KeepResult apply_keep_set(int slot,
                              const SwatchIdentity *keep,
                              std::size_t keep_count) noexcept;

    // ---- Cross-session JSON persistence --------------------------
    //
    // Identity is keyed by `(submesh_name, token_name)`, both of
    // which survive engine token-id rebucketing and game patches:
    //
    //   * `submesh_name` is captured from
    //     `SkinnedMeshMaterialWrapper +0x28` -- a pac asset name,
    //     stable across game sessions AND patches.
    //   * `token_name` is the shader-property reflection name (e.g.
    //     `_dyeingColorMaskR`); the engine's string interner
    //     re-buckets numeric ids each session, but the names are
    //     baked into engine code.
    //
    // Match semantics on load: an entry is consumed when the live
    // setter captures a row with the same `(submesh_name, token)`
    // pair and the slot context is known (the publisher's apply
    // window). Entries are organized per-slot in JSON; the runtime
    // pending-overrides map keys by `(slot, submesh, token_id)`
    // for setter-side O(1) lookup.
    //
    // Independent of `DyeRecordInject::ChannelDye` (the ARMOR_MOD
    // copy path under JSON key `dye_mods`); these entries live
    // under `swatch_overrides`.
    //
    // Compact triple used by both the "user override" and
    // "captured default" persistence paths. The semantic meaning of
    // r/g/b depends on which list the entry lives in:
    //   * swatch_overrides: r/g/b is the user-picked colour; the
    //     substitute path writes this when override_active is true
    //     at load time.
    //   * swatch_defaults:  r/g/b is the engine-captured default
    //     (the value the engine would have written naturally).
    struct PersistEntry
    {
        std::string   submesh_name;   // e.g. "cd_phm_00_hel_00_0435_dec"
        std::string   token_name;     // e.g. "_dyeingColorMaskR"
        std::uint8_t  r = 0, g = 0, b = 0;
    };

    /// Snapshot every row the user has explicitly picked a colour
    /// for (override_active==true). Each entry's r/g/b is the user's
    /// pick. Used to populate the "swatch_overrides" JSON section.
    std::vector<PersistEntry> get_persistable_overrides(int slot) noexcept;

    /// Snapshot every row with a valid identity (submesh+token name
    /// resolvable). r/g/b is NOT populated -- the palette captures
    /// only the (submesh, token) structure so the JSON section can
    /// be a flat list of token names per submesh. Used to populate
    /// the "swatch_palette" JSON section -- the per-slot list of
    /// rows the user has seen, used to re-seed placeholder rows on
    /// preset switch back. Engine defaults are NOT persisted; they
    /// get re-captured live via capture_default_if_unset.
    std::vector<PersistEntry> get_persistable_palette(int slot) noexcept;

    /// Queue persisted user-override entries into the
    /// PendingOverrides map. The setter consults this map on every
    /// successful insert/match and auto-applies the saved RGB.
    /// Defaults snapshot does NOT need to go through PendingOverrides
    /// because rows are seeded directly with their engine values via
    /// populate_from_persisted's defaults phase.
    void restore_persisted_state(
        int slot, const std::vector<PersistEntry> &overrides) noexcept;

    /// Pre-seed the slot's swatch table from the persisted palette
    /// and user-override lists. Two passes:
    ///
    ///   1. Walk `palette` and allocate one placeholder row per
    ///      `(submesh, token)`. All rows start with
    ///      `override_active = false` and no captured default; the
    ///      engine fills `def_r/g/b` on its first matching write
    ///      via `capture_default_if_unset`.
    ///   2. Walk `overrides`. For each entry, find the row already
    ///      created above (matching submesh+token) and set
    ///      `ovr.r/g/b` and `override_active = true`. If no
    ///      matching palette row exists (an override was saved
    ///      without a palette entry), allocate a new placeholder.
    ///
    /// The slot is left LOCKED after seeding -- subsequent
    /// unrecognized submeshes still fall through; only matched
    /// identities get promoted on engine writes. Returns the number
    /// of rows seeded.
    std::size_t populate_from_persisted(
        int slot,
        const std::vector<PersistEntry> &palette,
        const std::vector<PersistEntry> &overrides) noexcept;

    // ---- Diagnostic dump --------------------------------------------
    //
    // Emit a tree-formatted snapshot of the slot's swatch state to
    // the logger so the full structure is reconstructible from a log
    // file alone (no need to open the UI to see what's there).
    // Output shape:
    //
    //   [swatch-dump] slot=N rows=M unique_submeshes=K
    //     submesh='cd_phm_00_hel_00_0377_01' tpl=0x3B6B rows=3
    //       _tintColorR (0x2F66) def=#808080 user=#A0805C [ACTIVE]
    //       _tintColorG (0x2F67) def=#909090 user=#A0805C [ACTIVE]
    //       _tintColorB (0x2F68) def=#A0A0A0 user=#A0805C [DEFAULT]
    //     submesh='(unnamed)' tpl=0x3ADC rows=...
    //       ...
    //
    // Tagged so log grep is easy. Default-off in shipping; calls
    // happen at slot-apply-end, preset save/load, and wipe events.
    void dump_slot(int slot) noexcept;

    /// Dump every slot back-to-back. Used by preset save/load to
    /// snapshot the full picture in one log block.
    void dump_all_slots() noexcept;

    /// Enforce the strict-init default: lock every slot so setter
    /// writes can't add rows outside an explicit Reinit cycle. Called
    /// once from `ColorOverride::init()` after construction, before
    /// PresetManager::load() runs. Idempotent.
    void lock_all_slots() noexcept;

    /// Diagnostic counts for a slot:
    ///   total            -- live rows in g_table (count)
    ///   promoted         -- rows with non-zero content_hash (real
    ///                       live identity, substitute can fire)
    ///   placeholders     -- rows with content_hash == 0 (seeded by
    ///                       populate_from_persisted, waiting for the
    ///                       engine to write a matching (submesh,
    ///                       token) pair)
    ///   active_overrides -- rows where SwatchOverride::override_active
    ///                       is true (user has picked a colour)
    struct SlotCounts
    {
        std::size_t total            = 0;
        std::size_t promoted         = 0;
        std::size_t placeholders     = 0;
        std::size_t active_overrides = 0;
    };
    SlotCounts slot_counts(int slot) noexcept;
}

// Per-slot DyeSlot storage exposed at the parent namespace so the
// picker can read `dye_state()[i].slot_enabled` and
// `dye_state()[i].swatches[s]` directly.
namespace Transmog::ColorOverride
{
    struct DyeSlot
    {
        std::array<SwatchTable::DyeRGB, k_dyeSwatchesPerSlot> swatches{};
        bool slot_enabled = false;
    };

    /// Canonical per-slot DyeSlot storage. `PickerState::slot_enabled`
    /// and `SwatchTable`'s per-row override fields all alias into
    /// this array.
    std::array<DyeSlot, k_slotCount> &dye_state() noexcept;

    /// "Advanced view" toggle for the dye picker UI. Session-only;
    /// not currently persisted across runs.
    bool dye_advanced_view_get() noexcept;
    void dye_advanced_view_set(bool v) noexcept;
}
