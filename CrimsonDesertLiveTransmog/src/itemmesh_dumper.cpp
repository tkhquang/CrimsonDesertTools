#include "itemmesh_dumper.hpp"

#include "aob_resolver.hpp"
#include "item_name_table.hpp"
#include "prefab_wrapper_swap.hpp"
#include "shared_state.hpp"
#include "transmog_map.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Transmog
{
    namespace
    {
        // The first call resolves both registry-holder absolute addresses and caches them for the lifetime of the
        // process. Neither carries an AOB cascade: a per-manager accessor is a byte-identical template clone that
        // differs only in its RIP displacement, so a global pattern can never be unique. The iteminfo holder
        // therefore comes from ItemNameTable's bounded call-graph walk, and the stringinfo holder IS the
        // StringInfoRegistry slot. Both holders dereference to a registry struct with the standard
        // `+0x08 = u32 count`, `+0x58 = QWORD entry-array` layout.
        //
        // The entry-array offset moves whenever the pa::StaticInfoManager2<> base changes width. The count sits ahead
        // of the growth point and stays put, so usually only the array offset needs a new value. A stale array offset
        // fails SILENTLY: the neighboring offset also holds a valid heap pointer, to a different small-stride array,
        // so no plausibility guard trips and the walk yields nothing useful. On patch day, verify both offsets against
        // live memory.
        constexpr ptrdiff_t OFF_REGISTRY_COUNT = 0x08;
        constexpr ptrdiff_t OFF_REGISTRY_ARRAY = 0x58;

        constexpr ptrdiff_t OFF_NAME = 0x08;
        constexpr ptrdiff_t OFF_META_SUB = 0x90;

        // The engine stores strings behind a "StringRef" descriptor: {char* ptr @+0x00, u32 len @+0x08}. The
        // characters are always heap-allocated, down to strings only a few characters long, so there is no inline/SSO
        // form to special-case.
        //
        // Two wrapper shapes reference a StringRef:
        //   - Item NAME wrappers (desc+0x08) ARE a StringRef directly.
        //   - stringinfo ICON wrappers are a larger object that stores a pointer to their StringRef at +0x18. For an
        //     inline icon wrapper, that pointer targets the wrapper's own +0x20. For an external one, it targets a
        //     StringRef elsewhere on the heap. A dereference of +0x18 resolves both forms. A fixed +0x20/+0x28 read
        //     only handles the inline form and reads garbage on the external one.
        constexpr ptrdiff_t OFF_WRAP_DESC = 0x18; // icon wrapper -> StringRef*
        constexpr ptrdiff_t OFF_REF_PTR = 0x00;   // StringRef: char*
        constexpr ptrdiff_t OFF_REF_LEN = 0x08;   // StringRef: u32 length

        // Item body-mesh chain. An item carries its actual per-rig mesh prefab names here, independent of the icon.
        // Several rig variants of one item share a single icon - Daeil_Band, Damian_Daeil_Band and OOngka_Daeil_Band
        // all expose ItemIcon_Prefab_cd_phm_... yet emit cd_phm / cd_phw / cd_pom meshes respectively - so the icon
        // alone collapses them onto the cd_phm prefab. desc+0x248 holds a rule-list pointer with its count at +0x250.
        // Each rule's mesh-slot array (u16 stringinfo slots) is at rule+0x00 with its count at rule+0x08.
        constexpr ptrdiff_t OFF_DESC_RULE_LIST = 0x248;
        constexpr ptrdiff_t OFF_DESC_RULE_COUNT = 0x250;
        constexpr ptrdiff_t RULE_STRIDE = 0x38;
        constexpr ptrdiff_t OFF_RULE_MESH_ARR = 0x00;
        constexpr ptrdiff_t OFF_RULE_MESH_COUNT = 0x08;
        constexpr uint32_t RULE_SCAN_CAP = 64; // bound on garbage counts

        // Per-body mesh variant entry list (desc+0x408), the structure the engine's variant resolver walks to render an
        // item's mesh for the wearer's body. Each 0x58-byte entry names one body variant of the item. The mesh handle
        // is reached via a pointer at entry+0x10 (deref -> handle whose low 16 bits index stringinfo). This is the
        // authoritative, per-item, noise-free source of every mesh an item uses across bodies (human male / orc /
        // female / NPC), which the shared, rig-stripped icon cannot enumerate - e.g. Kliff_Mask binds three rig meshes
        // here (`cd_phm_` / `cd_pom_` / `cd_phw_`) behind one `cd_phm_` icon. The engine also stores each entry's
        // wearer body-class token array at entry+0x40 with its count at +0x48. The dump links every entry's mesh
        // regardless of which body selects it, so it does not need those two.
        //
        // The list offset and its count offset move when a patch reshapes the item descriptor, and they move ALONE:
        // one past reshape pushed them forward while the neighboring rule list and variant-meta pointer stayed put,
        // so the growth is local and no other field predicts it. The entry stride 0x58 and the per-entry mesh pointer
        // at +0x10 do stay put across such a reshape.
        //
        // A stale pair fails SILENTLY: the read returns a non-pointer or an absurd count, resolve_variant_meshes
        // returns empty, and every target reports "variant meshes [<none>] absent from this slot's catalog" while the
        // catalog itself looks healthy. That is why `resolve_variant_list_offset` below probes rather than trusting
        // the nominal value.
        constexpr ptrdiff_t OFF_DESC_VARIANT_ENTRIES_NOMINAL = 0x418;
        constexpr ptrdiff_t OFF_DESC_VARIANT_PROBE_LOW = 0x380;
        constexpr ptrdiff_t OFF_DESC_VARIANT_PROBE_HIGH = 0x480;
        constexpr ptrdiff_t OFF_DESC_VARIANT_COUNT_DELTA = 0x08; // count sits immediately after the pointer
        constexpr ptrdiff_t VARIANT_ENTRY_STRIDE = 0x58;
        constexpr ptrdiff_t OFF_ENTRY_MESH = 0x10; // pointer into the variant table -> mesh handle
        constexpr uint32_t VARIANT_ENTRY_CAP = 32;

        // safe reads
        //
        // The `(value, bool& ok)` shape distinguishes a faulted read from a legitimate zero result, which matters at
        // call sites where 0 is a valid value (e.g. a slot index of 0). `memory::read` is the underlying
        // SEH-protected primitive. These adapters fold its `Result<T>` return into the local shape used by the
        // dumper's bounded walks.

        uintptr_t read_qword_safe(uintptr_t addr, bool &ok) noexcept
        {
            const auto v = DMK::memory::read<uintptr_t>(DMK::Address{addr});
            ok = v.has_value();
            return v.value_or(0);
        }

        uint32_t read_u32_safe(uintptr_t addr, bool &ok) noexcept
        {
            const auto v = DMK::memory::read<uint32_t>(DMK::Address{addr});
            ok = v.has_value();
            return v.value_or(0);
        }

        uint16_t read_u16_safe(uintptr_t addr, bool &ok) noexcept
        {
            const auto v = DMK::memory::read<uint16_t>(DMK::Address{addr});
            ok = v.has_value();
            return v.value_or(0);
        }

        size_t read_printable_into(const char *src, char *dst, size_t max_len) noexcept
        {
            size_t n = 0;
            __try
            {
                for (size_t i = 0; i < max_len; ++i)
                {
                    char c = src[i];
                    if (c == 0)
                        break;
                    // Reject control bytes (0x01..0x1F): a misaligned or torn heap read lands on pointer/length bytes
                    // that carry low control values, so a control byte aborts the read. Accept high bytes
                    // (0x80..0xFF), because some item name keys use UTF-8 - the Roman numerals in
                    // `Goblin_Merchant_Fabric_Armor_*` use `E2 85 A2..A5`. A printable-ASCII-only filter zeroes the
                    // whole name and drops the item from the prefab pass entirely. Such an item never reaches the
                    // exact/base maps, so its prefab loses its item link. This mirrors the name reader in
                    // item_name_table.cpp.
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        n = 0;
                        break;
                    }
                    dst[n++] = c;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                n = 0;
            }
            return n;
        }

        std::string read_cstr_safe(const char *src, size_t max_len) noexcept
        {
            char buf[512];
            const size_t cap = max_len < sizeof(buf) ? max_len : sizeof(buf);
            size_t n = read_printable_into(src, buf, cap);
            return std::string(buf, n);
        }

        // Read a StringRef descriptor {char* ptr @+0x00, u32 len @+0x08}.
        std::string read_string_ref(uintptr_t node) noexcept
        {
            if (!node)
                return {};
            bool ok = false;
            const uint32_t len = read_u32_safe(node + OFF_REF_LEN, ok);
            if (!ok || len == 0 || len > 0x10000)
                return {};
            const uintptr_t cstr = read_qword_safe(node + OFF_REF_PTR, ok);
            if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{cstr}))
                return {};
            return read_cstr_safe(reinterpret_cast<const char *>(cstr), len + 1);
        }

        // stringinfo ICON wrapper: the StringRef is reached via wrap+0x18. An inline wrapper points back at its own
        // +0x20 and an external one points elsewhere. Dereference wrap+0x18 so both layouts resolve.
        std::string read_wrapper_string(uintptr_t wrap) noexcept
        {
            if (!wrap)
                return {};
            bool ok = false;
            const uintptr_t node = read_qword_safe(wrap + OFF_WRAP_DESC, ok);
            if (!ok)
                return {};
            return read_string_ref(node);
        }

        // Distinguish icon strings that map to a real mesh prefab from those that are UI-texture-only. The engine
        // writes `ItemIcon_Prefab_<name>` for icons backed by a `.prefab` file (the rows the dumper wants to emit) and
        // `ItemIcon_<name>` (no `_Prefab_` infix) for icons that resolve to a UI texture bundle (quest dialogs,
        // abyss-gear sockets, money/trade UI, collection-album thumbnails, housing-UI icons, puzzle slots, and so on).
        // Two cd_* sub-families (`cd_questimage_*` and `cd_knowledgeimage_*`) are likewise UI-only, and the targeted
        // phantom-recovery pass must drop them first. Without that filter, the pass resolves unrelated quest-registry
        // strings against mesh-prefab names that overlap post-strip.
        bool starts_with_asset_prefix(std::string_view sl) noexcept;

        // True when the icon string does NOT encode a real mesh/world-object prefab - i.e. it is UI-texture-only
        // (quest dialogs, abyss-gear, skill/stat items, money/trade UI, memory chips, dev stubs, or a bespoke
        // `ItemIcon_<ItemName>` whose post-strip name is not a known asset family).
        //
        // An icon encodes a mesh when, after the strip of the `itemicon_`(`prefab_`) wrapper, the remainder begins with
        // a known asset-prefix family (the same accept-list the pool enrich pass uses:
        // cd_/gimmick_/collection_/craft_/puzzle_/background_/lamp_/fs_/docking_/item_rare_). The test uses the family,
        // not the `itemicon_prefab_` infix alone, and that is deliberate: gimmick/collection/craft/puzzle world-objects
        // own real meshes but reference them through the bare `itemicon_` form, so they must still count as prefabs.
        bool is_ui_only_icon(std::string_view full_icon_lower) noexcept
        {
            // `cd_questimage_*` / `cd_knowledgeimage_*` are UI-texture bundle entries with no underlying mesh.
            constexpr std::string_view cd_ui[] = {
                "cd_questimage_",
                "cd_knowledgeimage_",
            };
            for (const auto &p : cd_ui)
            {
                if (full_icon_lower.size() >= p.size() && full_icon_lower.substr(0, p.size()) == p)
                    return true;
            }
            // `itemicon_prefab_<cd_mesh>` is always a real mesh.
            constexpr std::string_view itemicon_mesh = "itemicon_prefab_";
            constexpr std::string_view itemicon_all = "itemicon_";
            std::string_view rest = full_icon_lower;
            if (rest.size() >= itemicon_mesh.size() && rest.substr(0, itemicon_mesh.size()) == itemicon_mesh)
                return false;
            if (rest.size() >= itemicon_all.size() && rest.substr(0, itemicon_all.size()) == itemicon_all)
                rest = rest.substr(itemicon_all.size());
            // Mesh iff the icon-derived name names a known asset family.
            return !starts_with_asset_prefix(rest);
        }

        std::string to_lower(std::string_view s)
        {
            std::string out;
            out.reserve(s.size());
            for (char c : s)
            {
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
                out.push_back(c);
            }
            return out;
        }

        // Build the pool from the prefab_wrapper_swap cached catalog. Engine-validated, zero garbage, but
        // vtable-filtered: walk_string_info accepts only one wrapper vtable identity, so it drops some legitimate cd_*
        // prefabs (bags, monsters, NPCs, accessories). The supplementary stringinfo walk below recovers those.
        std::size_t build_pool_from_catalog(std::set<std::string> &out)
        {
            auto &logger = DMK::log();
            if (!prefab_wrapper_swap::is_catalog_populated())
            {
                logger.warning("[itemprefab] prefab catalog not yet populated");
                return 0;
            }
            const auto &cat = prefab_wrapper_swap::slot_catalog(Transmog::TransmogSlot::Helm);
            std::size_t added = 0;
            for (const auto &e : cat)
            {
                if (e.name.empty())
                    continue;
                if (out.insert(to_lower(e.name)).second)
                    ++added;
            }
            return added;
        }

        // Collect every LIVE partprefab wrapper name across all slot catalogs (lowercased). Used to detect the helm
        // `_dd` runtime-suffix case: the swap/carrier path matches wrappers by EXACT name, and the engine instantiates
        // the default helm variant's wrapper as `<mesh>_dd` while the bare `<mesh>` exists only as a data/string entry
        // with no live wrapper. Linking items to the `_dd` name keeps this dump aligned with what carrier_defaults and
        // the swap resolver can actually match (cf. carrier_defaults.hpp Oongka-helm note + prefab_wrapper_swap.cpp
        // HELM_SLOT_ID "helm is the only pair with a suffix").
        std::size_t collect_live_wrapper_names(std::set<std::string> &out)
        {
            if (!prefab_wrapper_swap::is_catalog_populated())
                return 0;
            std::size_t added = 0;
            for (std::size_t s = 0; s < static_cast<std::size_t>(Transmog::TransmogSlot::Count); ++s)
            {
                const auto &cat = prefab_wrapper_swap::slot_catalog(static_cast<Transmog::TransmogSlot>(s));
                for (const auto &e : cat)
                {
                    if (!e.name.empty() && out.insert(to_lower(e.name)).second)
                        ++added;
                }
            }
            return added;
        }

        // Allow-list of asset-name prefixes for the stringinfo enrich pass. Each accepted family resolves to a real
        // `.prefab` file on disk. UI-only families (`itemicon_*`, `cd_questimage_*`, `cd_knowledgeimage_*`) are
        // rejected first, so the pool never contains texture-bundle entries that lack an underlying mesh.
        bool starts_with_asset_prefix(std::string_view sl) noexcept
        {
            // sl is already lowercased.
            if (sl.size() < 3)
                return false;
            // Reject UI-only cd_* sub-families first
            if (sl.size() >= 13 && sl.substr(0, 13) == "cd_questimage")
                return false;
            if (sl.size() >= 17 && sl.substr(0, 17) == "cd_knowledgeimage")
                return false;
            // Accept-list (each ends with an underscore so the test does not pick up name collisions like `cdkey_*` or
            // `gimmickspecial_*`).
            constexpr std::string_view accepts[] = {
                "cd_",
                "gimmick_",
                "collection_",
                "craft_",
                "puzzle_",
                "background_",
                "lamp_",
                "fs_phm_",
                "fs_phw_",
                "docking_",
                "item_rare_",
            };
            for (const auto &p : accepts)
            {
                if (sl.size() >= p.size() && sl.substr(0, p.size()) == p)
                    return true;
            }
            return false;
        }

        // Supplementary stringinfo walk without the vtable filter. It picks up the asset families the swap-catalog's
        // vtable gate drops AND the non-cd_ families (gimmick, collection, craft, puzzle, etc.) that
        // prefab_wrapper_swap does not index at all. This only enriches the dumper's pool. prefab_wrapper_swap stays
        // untouched, so the live swap path and the UI picker keep their scope.
        std::size_t enrich_pool_from_stringinfo(std::set<std::string> &out, uintptr_t string_arr, uint32_t stringCount)
        {
            bool ok = false;
            std::size_t added = 0;
            for (uint32_t i = 0; i < stringCount; ++i)
            {
                const uintptr_t wrap = read_qword_safe(string_arr + i * 8ull, ok);
                if (!ok || !wrap)
                    continue;
                const std::string s = read_wrapper_string(wrap);
                if (s.size() < 4)
                    continue;
                const std::string sl = to_lower(s);
                if (!starts_with_asset_prefix(sl))
                    continue;
                if (out.insert(sl).second)
                    ++added;
            }
            return added;
        }

        // Bounded byte-scan window for the residual asset-pool pass (enrich_pool_from_asset_pool). Crimson Desert's
        // heap-loaded asset strings (gimmick/collection/cd_ex_*/etc.) cluster in the low 0x01-0x20 VA band. A scan of
        // this window alone is fast and catches most of them. The wider full-VA walk in recover_phantoms_targeted
        // picks up the families that land outside it.
        constexpr uintptr_t ASSET_SCAN_START = 0x01000000;
        constexpr uintptr_t ASSET_SCAN_END = 0x20000000;

        bool is_prefab_char(char c) noexcept
        {
            unsigned char u = static_cast<unsigned char>(c);
            return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_';
        }

        // True at a position that looks like the start of a wanted asset-name string. Matches all the prefixes the
        // stringinfo enrich accepts (cd_/gimmick_/collection_/craft_/puzzle_/background_/lamp_/fs_/docking_/
        // item_rare_). The first letter matches case-insensitively. Later chars use the same set as `is_prefab_char`
        // plus a fast-fail.
        bool starts_with_asset_byte_prefix(const char *p, size_t avail) noexcept
        {
            if (avail < 4)
                return false;
            // cd_*
            if ((p[0] == 'c' || p[0] == 'C') && (p[1] == 'd' || p[1] == 'D') && p[2] == '_')
                return true;
            // fs_*
            if ((p[0] == 'f' || p[0] == 'F') && (p[1] == 's' || p[1] == 'S') && p[2] == '_')
                return true;
            if (avail < 7)
                return false;
            // gimmick_
            if ((p[0] == 'g' || p[0] == 'G') && std::memcmp(p + 1, "immick_", 7) == 0)
                return true;
            // gimmick_ must match first, because the avail<7 guard above already returned. craft_ (6 chars) and lamp_
            // (5 chars) are shorter, so each one gets its own length guard below.
            if (avail >= 6)
            {
                if ((p[0] == 'c' || p[0] == 'C') && std::memcmp(p + 1, "raft_", 5) == 0)
                    return true;
            }
            if (avail >= 5)
            {
                if ((p[0] == 'l' || p[0] == 'L') && std::memcmp(p + 1, "amp_", 4) == 0)
                    return true;
            }
            if (avail < 8)
                return false;
            if ((p[0] == 'd' || p[0] == 'D') && std::memcmp(p + 1, "ocking_", 7) == 0)
                return true;
            if (avail < 7)
                return false;
            if ((p[0] == 'p' || p[0] == 'P') && std::memcmp(p + 1, "uzzle_", 6) == 0)
                return true;
            if (avail < 11)
                return false;
            if ((p[0] == 'c' || p[0] == 'C') && std::memcmp(p + 1, "ollection_", 10) == 0)
                return true;
            if ((p[0] == 'b' || p[0] == 'B') && std::memcmp(p + 1, "ackground_", 10) == 0)
                return true;
            if (avail < 10)
                return false;
            if ((p[0] == 'i' || p[0] == 'I') && std::memcmp(p + 1, "tem_rare_", 9) == 0)
                return true;
            return false;
        }

        void scan_chunk_for_asset_strings(const char *buf, size_t n, std::set<std::string> &out)
        {
            if (n < 8)
                return;
            for (size_t i = 0; i + 3 < n; ++i)
            {
                if (!starts_with_asset_byte_prefix(buf + i, n - i))
                    continue;
                size_t e = i;
                while (e < n && is_prefab_char(buf[e]))
                {
                    ++e;
                    if (e - i >= 192)
                        break;
                }
                if (e - i >= 10)
                {
                    std::string s;
                    s.reserve(e - i);
                    for (size_t k = i; k < e; ++k)
                    {
                        char c = buf[k];
                        if (c >= 'A' && c <= 'Z')
                            c = static_cast<char>(c - 'A' + 'a');
                        s.push_back(c);
                    }
                    out.insert(std::move(s));
                }
                i = e;
            }
        }

        // Targeted phantom recovery. Once the pool passes above finish, an item whose icon_prefab is not in the pool
        // becomes a "phantom candidate". This walk crosses memory once and searches for the exact phantom strings
        // through a first-4-byte hash table - O(memory_size + N_phantoms) total. It catches a byte-scan miss where
        // the asset string lives in a region the chunked walker mis-handled, or where the prefix gate did not match.
        std::size_t
        recover_phantoms_targeted(std::set<std::string> &pool, const std::vector<std::string> &phantom_names)
        {
            if (phantom_names.empty())
                return 0;

            // Index phantoms by first 4 bytes (lowercased).
            std::unordered_map<uint32_t, std::vector<size_t>> by_first4;
            by_first4.reserve(phantom_names.size() * 2);
            for (size_t i = 0; i < phantom_names.size(); ++i)
            {
                if (phantom_names[i].size() < 4)
                    continue;
                uint32_t k = 0;
                std::memcpy(&k, phantom_names[i].data(), 4);
                by_first4[k].push_back(i);
            }
            if (by_first4.empty())
                return 0;

            std::vector<uint8_t> found(phantom_names.size(), 0);
            std::size_t recovered = 0;

            // Targeted recovery walks the FULL user address range, because asset string heap addresses shift between
            // reloads: the same string lands in a low or a high VA band, depending on allocation order. The per-byte
            // cost is one uint32 hash lookup, which is cheap enough to cover the whole VA in a single pass.
            constexpr size_t chunk = 0x100000;
            std::vector<char> scratch(chunk);
            // whole_process() is the one Region factory that queries no loader state. It reads the system's own
            // minimum and maximum application addresses, so the floor is not an assumption either.
            const DMK::Region user_space = DMK::Region::whole_process();
            const uintptr_t addrEnd = user_space.end().raw();
            uintptr_t addr = user_space.base.raw();
            while (addr < addrEnd)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
                    break;
                const DWORD readable =
                    PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY;
                if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) != 0 && (mbi.Protect & PAGE_GUARD) == 0)
                {
                    const uintptr_t bs = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                    const size_t total = static_cast<size_t>(mbi.RegionSize);
                    for (size_t off = 0; off < total; off += chunk)
                    {
                        const size_t got = std::min<size_t>(chunk, total - off);
                        if (got < 4)
                            continue;
                        // read_into fails the whole span, which is exactly the "skip this chunk" contract this walk
                        // wants, and it reports the faulting address in Error::detail.
                        if (!DMK::memory::read_into(
                                DMK::Address{bs + off},
                                std::as_writable_bytes(std::span{scratch.data(), got})
                            ))
                            continue;
                        // Lowercase the chunk in-place for case-insensitive search (phantom names are already
                        // lowercase).
                        for (size_t k = 0; k < got; ++k)
                        {
                            char c = scratch[k];
                            if (c >= 'A' && c <= 'Z')
                                scratch[k] = static_cast<char>(c - 'A' + 'a');
                        }
                        for (size_t pos = 0; pos + 4 <= got; ++pos)
                        {
                            uint32_t key = 0;
                            std::memcpy(&key, scratch.data() + pos, 4);
                            auto it = by_first4.find(key);
                            if (it == by_first4.end())
                                continue;
                            for (size_t pi : it->second)
                            {
                                if (found[pi])
                                    continue;
                                const auto &target = phantom_names[pi];
                                if (pos + target.size() > got)
                                    continue;
                                if (std::memcmp(scratch.data() + pos, target.data(), target.size()) == 0)
                                {
                                    if (pool.insert(target).second)
                                        ++recovered;
                                    found[pi] = 1;
                                }
                            }
                        }
                    }
                }
                const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (next <= addr)
                    break;
                addr = next;
            }
            return recovered;
        }

        std::size_t enrich_pool_from_asset_pool(std::set<std::string> &out)
        {
            const std::size_t before = out.size();
            constexpr size_t chunk = 0x100000;
            std::vector<char> scratch(chunk);
            uintptr_t addr = ASSET_SCAN_START;
            while (addr < ASSET_SCAN_END)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
                    break;
                const DWORD readable =
                    PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY;
                // Scan any committed readable region within the bounded VA window (ASSET_SCAN_START..ASSET_SCAN_END).
                // MEM_PRIVATE is welcome here, because Crimson Desert's heap-loaded asset strings
                // (gimmick/collection/`cd_ex_*`/etc.) live in mid-size MEM_PRIVATE regions clustered in the 0x01-0x20
                // VA band. PAGE_GUARD pages fault on read, so skip them. MEM_IMAGE is included and is harmless,
                // because asset names rarely appear in .rdata literals.
                if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) != 0 && (mbi.Protect & PAGE_GUARD) == 0)
                {
                    const uintptr_t bs = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                    const size_t total = static_cast<size_t>(mbi.RegionSize);
                    for (size_t off = 0; off < total; off += chunk)
                    {
                        const size_t want = std::min<size_t>(chunk, total - off);
                        if (DMK::memory::read_into(
                                DMK::Address{bs + off},
                                std::as_writable_bytes(std::span{scratch.data(), want})
                            ))
                            scan_chunk_for_asset_strings(scratch.data(), want, out);
                    }
                }
                const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (next <= addr)
                    break;
                addr = next;
            }
            return out.size() - before;
        }

        // TSV columns are tab-delimited and rows are newline-delimited, so a control character inside a name splits or
        // shifts the row when written. A cached asset string can carry an embedded CR/LF. Strip TAB/CR/LF so the name
        // can neither corrupt a row nor differ from its clean twin once both are pooled.
        std::string tsv_sanitize(std::string_view s)
        {
            std::string result;
            result.reserve(s.size());
            for (const char c : s)
            {
                if (c != '\t' && c != '\r' && c != '\n')
                    result.push_back(c);
            }
            return result;
        }

        std::string extract_base_prefix(std::string_view icon) noexcept
        {
            if (icon.size() < 5)
                return std::string(icon);
            for (int i = static_cast<int>(icon.size()) - 4; i >= 1; --i)
            {
                if (icon[i - 1] != '_')
                    continue;
                bool digit = true;
                for (int k = 0; k < 4; ++k)
                {
                    char c = icon[i + k];
                    if (c < '0' || c > '9')
                    {
                        digit = false;
                        break;
                    }
                }
                if (digit)
                    return std::string(icon.substr(0, i + 4));
            }
            return std::string(icon);
        }

        // Strip a 3-character rig prefix (`cd_phm_` / `cd_phw_` / `cd_pom_` / `cd_nhm_` ...) so two rig variants of the
        // same mesh compare equal. Returns the input unchanged when it is not in the `cd_???_` shape, so model-keyed
        // prefabs (`cd_m0001_...`) keep their prefix and only collide with an exact twin.
        std::string_view strip_rig_prefix(std::string_view s) noexcept
        {
            if (s.size() >= 7 && s[0] == 'c' && s[1] == 'd' && s[2] == '_' && s[6] == '_')
                return s.substr(7);
            return s;
        }

        // Resolve the item's primary body-mesh prefab from its rule chain. With a non-empty @p icon_stem, it returns
        // the mesh whose rig-stripped name equals it - the same logical item in its actual wearer rig - which lets a
        // rig variant link to cd_phw / cd_pom instead of the shared cd_phm icon. A sub-part mesh (e.g.
        // `..._wire_0001_r`) has a different stem, so the walk drops it. For an ordinary item the matching mesh
        // equals the icon prefab, so the result is identical and only a genuine rig variant changes.
        //
        // When @p icon_stem is empty - the item's icon string is missing or a bare `ItemIcon_` stub, so there is
        // nothing to match against - fall back to the first asset-prefab mesh the rule chain names. The item still
        // owns a real mesh there, so this links it to its prefab instead of an orphan row. It returns "" only when
        // no usable mesh turns up, and the caller then keeps the icon-derived prefab.
        //
        // Not noexcept: it builds std::string values, so a bad_alloc must stay propagatable.
        std::string
        resolve_rule_body_mesh(uintptr_t desc, uintptr_t string_arr, uint32_t stringCount, std::string_view icon_stem)
        {
            if (!desc || !string_arr)
                return {};
            bool ok = false;
            const uintptr_t rule_list = read_qword_safe(desc + OFF_DESC_RULE_LIST, ok);
            const uint32_t rule_count = read_u32_safe(desc + OFF_DESC_RULE_COUNT, ok);
            if (!rule_list || rule_count == 0 || rule_count > RULE_SCAN_CAP)
                return {};
            std::string first_body_mesh;
            for (uint32_t r = 0; r < rule_count; ++r)
            {
                const uintptr_t rule = rule_list + static_cast<uintptr_t>(r) * RULE_STRIDE;
                const uintptr_t mesh_arr = read_qword_safe(rule + OFF_RULE_MESH_ARR, ok);
                const uint32_t mesh_count = read_u32_safe(rule + OFF_RULE_MESH_COUNT, ok);
                if (!mesh_arr || mesh_count == 0 || mesh_count > RULE_SCAN_CAP)
                    continue;
                for (uint32_t m = 0; m < mesh_count; ++m)
                {
                    const uint16_t mslot = read_u16_safe(mesh_arr + static_cast<uintptr_t>(m) * 2, ok);
                    if (!ok || mslot == 0xFFFF || mslot >= stringCount)
                        continue;
                    const uintptr_t wrap = read_qword_safe(string_arr + static_cast<uintptr_t>(mslot) * 8, ok);
                    std::string mesh = to_lower(read_wrapper_string(wrap));
                    if (mesh.empty())
                        continue;
                    if (!icon_stem.empty())
                    {
                        if (strip_rig_prefix(mesh) == icon_stem)
                            return mesh;
                    }
                    else if (first_body_mesh.empty() && starts_with_asset_prefix(mesh))
                    {
                        first_body_mesh = std::move(mesh);
                    }
                }
            }
            return first_body_mesh;
        }

        // per-item record collected from iteminfo/stringinfo

        // A garbage byte fragment the "cd_"-prefix pool walk picks up: a body-rig prefix (cd_phm_ / cd_pom_ / ... which
        // always name a body PART) directly followed by a short digits-then-letters token and nothing else - e.g.
        // `cd_pom_0jl`, `cd_phm_00fv`. These are exe-static byte fragments, where the rig-shaped text runs straight
        // into binary data. A real body-rig mesh carries a `_<NN>_<part>_` or `_m<NNNN>_` structure, so it never has
        // this shape, and no fragment of this shape ever links to an item. Used to skip them at emission so the dump
        // stays clean.
        bool is_junk_rig_fragment(std::string_view name) noexcept
        {
            static constexpr std::string_view rigs[] = {
                "cd_phm_",
                "cd_phw_",
                "cd_pom_",
                "cd_pgm_",
                "cd_pdm_",
                "cd_ptm_",
                "cd_nhm_",
                "cd_ndm_",
            };
            std::string_view tail;
            for (const auto &rig : rigs)
            {
                if (name.size() > rig.size() && name.substr(0, rig.size()) == rig)
                {
                    tail = name.substr(rig.size());
                    break;
                }
            }
            if (tail.empty())
                return false;
            // Junk iff the tail is one digit-run followed by one letter-run and nothing else. A real mesh's tail has an
            // underscore here (`00_ub_...`) or leads with a model letter (`m0001_...`), so it fails one of
            // these checks.
            std::size_t i = 0;
            while (i < tail.size() && tail[i] >= '0' && tail[i] <= '9')
                ++i;
            if (i == 0 || i == tail.size())
                return false;
            for (std::size_t j = i; j < tail.size(); ++j)
                if (tail[j] < 'a' || tail[j] > 'z')
                    return false;
            return true;
        }

        // Resolve EVERY distinct mesh an item uses across bodies, from the per-body variant entry list at the probed
        // variant-list offset - the authoritative linkage source. Each entry's mesh (via entry+OFF_ENTRY_MESH ->
        // handle -> stringinfo slot) is one body variant of the item (human male / orc / female / NPC), all
        // item-specific and noise-free. The shared, rig-stripped icon cannot enumerate them. The walk drops the
        // ground-drop mesh (`..._dropitem_...`), because it is a shared prop rather than a wearer variant. It returns
        // the deduplicated meshes in entry order, where the first is the item's default mesh, and it returns empty
        // for an item with no variant list, which then falls back to its icon-derived prefab.
        /**
         * @brief Does `off` look like the variant-entry list on this descriptor?
         * @details It validates the whole shape, not just the pointer: a plausible entries pointer, a sane count, and
         *          a first entry whose mesh handle indexes stringinfo. A neighboring field can satisfy any one of
         *          those on its own. All three together are what make the probe safe to trust.
         */
        bool variant_list_shape_ok(uintptr_t desc, ptrdiff_t off, uint32_t stringCount) noexcept
        {
            bool ok = false;
            const uintptr_t entries = read_qword_safe(desc + off, ok);
            if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{entries}))
                return false;
            const uint32_t count = read_u32_safe(desc + off + OFF_DESC_VARIANT_COUNT_DELTA, ok);
            if (!ok || count == 0 || count > VARIANT_ENTRY_CAP)
                return false;
            const uintptr_t mesh_ptr = read_qword_safe(entries + OFF_ENTRY_MESH, ok);
            if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{mesh_ptr}))
                return false;
            const uintptr_t handle = read_qword_safe(mesh_ptr, ok);
            if (!ok)
                return false;
            const uint16_t slot = static_cast<uint16_t>(handle & 0xFFFFu);
            return slot != 0xFFFF && slot < stringCount;
        }

        /**
         * @brief Resolve the variant-list offset once per session, and self-heal across a descriptor reshape.
         * @details The probe tries the nominal value first, which is what a healthy build uses. When a patch moves
         *          the field, the probe walks a bounded window and adopts the first offset that validates on several
         *          DIFFERENT items - several, because one item can carry a neighboring field that passes by
         *          coincidence. Drift reaches the log as a WARNING even though it self-corrected, so patch day is
         *          visible rather than silent.
         */
        ptrdiff_t resolve_variant_list_offset(uintptr_t item_arr, uint32_t itemCount, uint32_t stringCount) noexcept
        {
            auto &logger = DMK::log();
            constexpr uint32_t probe_samples = 8;    // distinct items an offset must satisfy
            constexpr uint32_t probe_scan_cap = 512; // descriptors examined before giving up

            const auto score = [&](ptrdiff_t off) noexcept
            {
                uint32_t hits = 0;
                bool ok = false;
                for (uint32_t id = 0; id < (std::min)(itemCount, probe_scan_cap) && hits < probe_samples; ++id)
                {
                    const uintptr_t desc = read_qword_safe(item_arr + static_cast<uintptr_t>(id) * 8, ok);
                    if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{desc}))
                        continue;
                    if (variant_list_shape_ok(desc, off, stringCount))
                        ++hits;
                }
                return hits;
            };

            if (score(OFF_DESC_VARIANT_ENTRIES_NOMINAL) >= probe_samples)
                return OFF_DESC_VARIANT_ENTRIES_NOMINAL;

            for (ptrdiff_t off = OFF_DESC_VARIANT_PROBE_LOW; off <= OFF_DESC_VARIANT_PROBE_HIGH; off += 8)
            {
                if (off == OFF_DESC_VARIANT_ENTRIES_NOMINAL)
                    continue;
                if (score(off) >= probe_samples)
                {
                    (void)logger.try_log(
                        DMK::LogLevel::Warning,
                        "[itemmesh] variant list DRIFTED: nominal {:#x} no longer validates, using {:#x} "
                        "({:+#x}). Update OFF_DESC_VARIANT_ENTRIES_NOMINAL in itemmesh_dumper.cpp.",
                        OFF_DESC_VARIANT_ENTRIES_NOMINAL,
                        off,
                        off - OFF_DESC_VARIANT_ENTRIES_NOMINAL
                    );
                    return off;
                }
            }

            (void)logger.try_log(
                DMK::LogLevel::Warning,
                "[itemmesh] variant list NOT FOUND in {:#x}..{:#x} - per-item mesh derivation is "
                "disabled, so targets fall back to the carrier's own visual.",
                OFF_DESC_VARIANT_PROBE_LOW,
                OFF_DESC_VARIANT_PROBE_HIGH
            );
            return 0;
        }

        /**
         * @brief Session cache for the probed offset.
         * @details Both the solver and the TSV dump walk the same descriptors, so the probe runs once rather than
         *          once per caller.
         * @note Not noexcept: std::call_once reports a failed first call through std::system_error.
         */
        ptrdiff_t cached_variant_list_offset(uintptr_t item_arr, uint32_t itemCount, uint32_t stringCount)
        {
            static std::once_flag once;
            static ptrdiff_t s_off = 0;
            std::call_once(once, [&] { s_off = resolve_variant_list_offset(item_arr, itemCount, stringCount); });
            return s_off;
        }

        // Not noexcept: it builds a std::vector<std::string>, so a bad_alloc must stay propagatable.
        std::vector<std::string>
        resolve_variant_meshes(uintptr_t desc, uintptr_t string_arr, uint32_t stringCount, ptrdiff_t variant_list_off)
        {
            std::vector<std::string> meshes;
            if (!desc || !string_arr || variant_list_off == 0)
                return meshes;
            bool ok = false;
            const uintptr_t entries = read_qword_safe(desc + variant_list_off, ok);
            const uint32_t count = read_u32_safe(desc + variant_list_off + OFF_DESC_VARIANT_COUNT_DELTA, ok);
            if (!entries || count == 0 || count > VARIANT_ENTRY_CAP)
                return meshes;
            for (uint32_t e = 0; e < count; ++e)
            {
                const uintptr_t entry = entries + static_cast<uintptr_t>(e) * VARIANT_ENTRY_STRIDE;
                // entry+OFF_ENTRY_MESH points into the variant table at the entry's mesh handle. Deref it to get the
                // handle whose low 16 bits index stringinfo.
                const uintptr_t mesh_ptr = read_qword_safe(entry + OFF_ENTRY_MESH, ok);
                if (!mesh_ptr)
                    continue;
                const uintptr_t handle = read_qword_safe(mesh_ptr, ok);
                if (!ok)
                    continue;
                const uint16_t slot = static_cast<uint16_t>(handle & 0xFFFFu);
                if (slot == 0xFFFF || slot >= stringCount)
                    continue;
                const uintptr_t wrap = read_qword_safe(string_arr + static_cast<uintptr_t>(slot) * 8, ok);
                std::string mesh = to_lower(read_wrapper_string(wrap));
                if (mesh.empty() || !starts_with_asset_prefix(mesh) || mesh.find("dropitem") != std::string::npos)
                    continue;
                if (std::find(meshes.begin(), meshes.end(), mesh) == meshes.end())
                    meshes.push_back(std::move(mesh));
            }
            return meshes;
        }

        struct ItemEntry
        {
            uint32_t runtime_idx;
            std::string internal_name;
            uint16_t icon_slot;
            std::string full_icon;   // raw stringinfo string, with or without the ItemIcon_Prefab_ prefix
            std::string icon_prefab; // lowercased, prefix stripped
            std::string base;        // last `_NNNN`-anchored prefix of icon_prefab
            // Distinct player rig meshes (Kliff / Oongka / Damiane) recovered from the variant entry list. Empty when
            // the item has none. Links the male / orc / female body variants the icon-derived prefab cannot pair.
            std::vector<std::string> variant_meshes;
        };
    } // namespace

    std::vector<std::string> variant_meshes_for_item(std::uint16_t item_id) noexcept
    {
        // Resolve the iteminfo + stringinfo registries once (same holders dump_itemmesh_tsv uses) and cache them -
        // they are stable for the session. Retry the AOB resolve while it fails (e.g. called before the
        // world/registries exist) so an early miss does not permanently disable the solver.
        static std::mutex s_registry_mtx;
        static uintptr_t s_item_arr = 0;
        static uintptr_t s_string_arr = 0;
        static uint32_t s_item_count = 0;
        static uint32_t s_string_count = 0;
        static bool s_ready = false;

        // Snapshot the cached registry handles under the lock so the per-item read below never races the one-shot
        // initialization on another thread. This function runs on both the game thread and the UI thread. The cached
        // statics are only ever written here, under this same lock.
        uintptr_t item_arr = 0;
        uintptr_t string_arr = 0;
        uint32_t itemCount = 0;
        uint32_t stringCount = 0;
        {
            std::scoped_lock lk(s_registry_mtx);
            if (!s_ready)
            {
                const uintptr_t iteminfo_holder_addr = ItemNameTable::instance().iteminfo_holder_addr();
                const uintptr_t stringinfo_holder_addr = anchor_address(AnchorId::StringInfoRegistry);
                bool ok = false;
                const uintptr_t iteminfo_mgr = iteminfo_holder_addr ? read_qword_safe(iteminfo_holder_addr, ok) : 0;
                const uintptr_t stringinfo_mgr =
                    stringinfo_holder_addr ? read_qword_safe(stringinfo_holder_addr, ok) : 0;
                if (iteminfo_mgr && stringinfo_mgr)
                {
                    s_item_count = read_u32_safe(iteminfo_mgr + OFF_REGISTRY_COUNT, ok);
                    s_item_arr = read_qword_safe(iteminfo_mgr + OFF_REGISTRY_ARRAY, ok);
                    s_string_count = read_u32_safe(stringinfo_mgr + OFF_REGISTRY_COUNT, ok);
                    s_string_arr = read_qword_safe(stringinfo_mgr + OFF_REGISTRY_ARRAY, ok);
                    s_ready = s_item_arr && s_string_arr && s_item_count && s_string_count;
                }
            }
            if (!s_ready)
                return {};
            item_arr = s_item_arr;
            string_arr = s_string_arr;
            itemCount = s_item_count;
            stringCount = s_string_count;
        }
        if (item_id >= itemCount)
            return {};

        bool ok = false;
        const uintptr_t desc = read_qword_safe(item_arr + static_cast<uint64_t>(item_id) * 8, ok);
        if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{desc}))
            return {};

        // The solver builds strings and a vector, and this boundary is noexcept, so contain the allocation failure
        // here rather than let it terminate the process.
        try
        {
            // Authoritative per-body variant list. Empty for a single-rig item -> fall back to the rule-chain primary
            // body mesh so the result is non-empty for any item that owns a real mesh.
            std::vector<std::string> meshes = resolve_variant_meshes(
                desc,
                string_arr,
                stringCount,
                cached_variant_list_offset(item_arr, itemCount, stringCount)
            );
            if (meshes.empty())
            {
                std::string primary = resolve_rule_body_mesh(desc, string_arr, stringCount, std::string_view{});
                if (!primary.empty())
                    meshes.push_back(std::move(primary));
            }
            return meshes;
        }
        catch (...)
        {
            return {};
        }
    }

    void dump_itemmesh_tsv(std::stop_token stop)
    {
        auto &logger = DMK::log();

        // Wait on the swap catalog before the prefab pool sources anything from it. The catalog pass reads
        // prefab_wrapper_swap's slot catalog, which populate_slot_catalogs() builds after a world load (on the
        // save-load tick, or lazily on first overlay open). Both dump triggers spawn this routine in a detached
        // thread the instant the dump flag is set, which can beat that build and leave the pool incomplete.
        //
        // Wait for the catalog rather than cap the wait: there is no valid timeout here. A player can sit at the main
        // menu for days with no world and therefore no catalog, so any cap skips the dump, or degrades it, for a state
        // that is normal and not failed. A poll until the catalog exists yields a complete dump whenever the world
        // finally loads, however long that takes. The wait is free, because this already runs off the main thread, and
        // the thread is reclaimed at process exit if a world is never entered.
        if (!prefab_wrapper_swap::is_catalog_populated())
        {
            logger.info("[itemprefab] waiting for swap catalog before dumping");
            constexpr DWORD catalog_poll_ms = 500;
            while (!prefab_wrapper_swap::is_catalog_populated())
            {
                // The catalog only populates once a world loads, so at the main menu this waits forever. Polling
                // the stop token is what lets teardown reclaim this worker instead of the module staying mapped.
                if (stop.stop_requested())
                {
                    logger.info("[itemprefab] dump abandoned: shutdown requested while waiting for the catalog");
                    return;
                }
                Sleep(catalog_poll_ms);
            }
        }

        // Resolve both registry-holder absolute addresses. Neither carries an AOB cascade: the engine's per-manager
        // accessors are byte-identical template clones that differ only in their RIP displacement, so a global
        // pattern cut from one matches every manager at once. The iteminfo holder comes from ItemNameTable's bounded
        // call-graph walk, and the stringinfo holder IS the StringInfoRegistry slot - the same global under two
        // names. One indirection yields the registry struct (header, then the count at OFF_REGISTRY_COUNT, then the
        // entry array at OFF_REGISTRY_ARRAY).
        const uintptr_t iteminfo_holder_addr = ItemNameTable::instance().iteminfo_holder_addr();
        const uintptr_t stringinfo_holder_addr = anchor_address(AnchorId::StringInfoRegistry);
        if (!iteminfo_holder_addr || !stringinfo_holder_addr)
        {
            logger.warning(
                "[itemprefab] holder AOB resolve failed: iteminfo=0x{:X} stringinfo=0x{:X}",
                iteminfo_holder_addr,
                stringinfo_holder_addr
            );
            return;
        }

        // `ok` is reused across these reads and its intermediate values are deliberately ignored: a faulted read yields
        // 0 via value_or(0), which the explicit null/zero checks below already reject.
        bool ok = false;
        const uintptr_t iteminfo_mgr = read_qword_safe(iteminfo_holder_addr, ok);
        const uintptr_t stringinfo_mgr = read_qword_safe(stringinfo_holder_addr, ok);
        if (!iteminfo_mgr || !stringinfo_mgr)
        {
            logger.warning(
                "[itemprefab] registry holder null: iteminfo=0x{:X} stringinfo=0x{:X}",
                iteminfo_mgr,
                stringinfo_mgr
            );
            return;
        }
        const uint32_t itemCount = read_u32_safe(iteminfo_mgr + OFF_REGISTRY_COUNT, ok);
        const uintptr_t item_arr = read_qword_safe(iteminfo_mgr + OFF_REGISTRY_ARRAY, ok);
        const uint32_t stringCount = read_u32_safe(stringinfo_mgr + OFF_REGISTRY_COUNT, ok);
        const uintptr_t string_arr = read_qword_safe(stringinfo_mgr + OFF_REGISTRY_ARRAY, ok);
        logger.info(
            "[itemprefab] iteminfo: count={} arr=0x{:X}  stringinfo: count={} arr=0x{:X}",
            itemCount,
            item_arr,
            stringCount,
            string_arr
        );
        if (itemCount == 0 || !item_arr || stringCount == 0 || !string_arr)
        {
            logger.warning("[itemprefab] one of the registries is empty");
            return;
        }

        // Four pool sources feed one set: the prefab_wrapper_swap cached catalog, a supplementary stringinfo walk
        // for the cd_* entries the swap-catalog's vtable filter dropped, the unfiltered AppearanceTableLoader
        // registry, and a bounded-window asset-pool byte-scan for the residue. The loader registry goes in
        // unfiltered because its own walker keys on the body-mesh slot tag and drops every entry without one, while
        // the dump wants them all for the gimmick/collection/lamp/puzzle families.
        const auto t0 = std::chrono::steady_clock::now();
        std::set<std::string> pool_set;
        const auto from_catalog = build_pool_from_catalog(pool_set);
        const auto from_stringinfo = enrich_pool_from_stringinfo(pool_set, string_arr, stringCount);
        std::size_t from_loader = 0;
        prefab_wrapper_swap::for_each_loader_prefab_name(
            [&](std::string_view name)
            {
                if (pool_set.insert(to_lower(name)).second)
                    ++from_loader;
            }
        );
        const auto from_asset_pool = enrich_pool_from_asset_pool(pool_set);
        const auto t1 = std::chrono::steady_clock::now();
        logger.info(
            "[itemprefab] pool pre-targeted: {} catalog + {} stringinfo + "
            "{} loader-reg + {} asset-pool = {} total ({} ms)",
            from_catalog,
            from_stringinfo,
            from_loader,
            from_asset_pool,
            pool_set.size(),
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
        );
        if (pool_set.empty())
        {
            logger.warning("[itemprefab] prefab pool empty - aborting dump.");
            return;
        }

        // Live partprefab wrapper names (from the swap catalogs). The item walk consults them to link an item to the
        // `_dd` helm-variant wrapper the swap/carrier path actually matches, and the emit loop consults them to drop
        // the dead bare data-name.
        std::set<std::string> live_wrapper_set;
        const auto live_wrapper_count = collect_live_wrapper_names(live_wrapper_set);
        logger.info("[itemprefab] live-wrapper set: {} names (for `_dd` helm-suffix linking)", live_wrapper_count);

        // Walk iteminfo and collect item entries with their icon-derived prefab and base. Emission runs later from
        // the pool's perspective, so this walk only collects.
        std::vector<ItemEntry> items;
        items.reserve(itemCount);
        uint32_t skipped = 0;
        for (uint32_t id = 0; id < itemCount; ++id)
        {
            const uintptr_t desc = read_qword_safe(item_arr + id * 8ull, ok);
            if (!desc)
            {
                ++skipped;
                continue;
            }
            const uintptr_t name_wrap = read_qword_safe(desc + OFF_NAME, ok);
            // The name wrapper IS a StringRef directly (no icon-style +0x18 indirection), so read it as one.
            std::string internal_name = read_string_ref(name_wrap);
            if (internal_name.empty())
            {
                // Recover entries whose StringRef length field is unreadable or zero while the character pointer is
                // still valid: read the pointer target with a fixed bound. read_cstr_safe stops at the first
                // non-printable byte, so the bound cannot overrun.
                const uintptr_t alt_ptr = read_qword_safe(name_wrap, ok);
                if (ok && DMK::memory::is_plausible_ptr(DMK::Address{alt_ptr}))
                    internal_name = read_cstr_safe(reinterpret_cast<const char *>(alt_ptr), 128);
            }
            if (internal_name.empty())
            {
                ++skipped;
                continue;
            }
            const uintptr_t meta_sub = read_qword_safe(desc + OFF_META_SUB, ok);
            if (!meta_sub)
            {
                ++skipped;
                continue;
            }
            const uint16_t slot = read_u16_safe(meta_sub, ok);
            if (!ok || slot == 0xFFFF || slot >= stringCount)
            {
                ++skipped;
                continue;
            }
            const uintptr_t wrap = read_qword_safe(string_arr + slot * 8ull, ok);
            const std::string full = read_wrapper_string(wrap);
            if (full.empty())
            {
                ++skipped;
                continue;
            }

            // Case-insensitive prefix strip. Icons come in three flavors:
            //   ItemIcon_Prefab_<cd_mesh>                         - body-mesh prefabs (cd_*)
            //   ItemIcon_<gimmick|collection|craft|puzzle>_<name> - world-object prefabs that share the icon
            //                                                        namespace but use a different mesh-prefab family
            //   itemicon_quest|abyssgear_*                        - UI-only (no mesh)
            // Strip `itemicon_prefab_` first (cd_*), else strip only `itemicon_`, so the remainder is the underlying
            // mesh name (e.g. `collection_prop_doll_0001`) that the pool indexes. Otherwise items resolve to
            // themselves as phantoms.
            std::string full_lower = to_lower(full);
            std::string_view icon_prefab = full_lower;
            constexpr std::string_view long_prefix = "itemicon_prefab_";
            constexpr std::string_view short_prefix = "itemicon_";
            // `itemicon_prefab_<mesh>` carries the real mesh name. A bare `itemicon_<name>` (e.g.
            // `ItemIcon_Lantern_On`) does not, so its stripped remainder is a non-mesh item name that the dump must
            // not trust as a prefab.
            bool is_prefab_icon = false;
            if (icon_prefab.size() >= long_prefix.size() && icon_prefab.substr(0, long_prefix.size()) == long_prefix)
            {
                icon_prefab = icon_prefab.substr(long_prefix.size());
                is_prefab_icon = true;
            }
            else if (icon_prefab.size() >= short_prefix.size() &&
                     icon_prefab.substr(0, short_prefix.size()) == short_prefix)
            {
                icon_prefab = icon_prefab.substr(short_prefix.size());
            }
            // Prefer the item's actual rig mesh over the icon-derived prefab. The icon is shared across rig variants,
            // so the rule chain is the only place the real cd_phw / cd_pom mesh appears. For an ordinary item the rule
            // mesh equals the icon prefab and this is a no-op.
            // The variant entry list is the authoritative source of every mesh the item uses across bodies. Resolve
            // it once and reuse it for the primary-prefab recovery below and for the per-body-variant linkage.
            std::vector<std::string> variant_meshes = resolve_variant_meshes(
                desc,
                string_arr,
                stringCount,
                cached_variant_list_offset(item_arr, itemCount, stringCount)
            );

            std::string body_mesh =
                resolve_rule_body_mesh(desc, string_arr, stringCount, strip_rig_prefix(icon_prefab));
            std::string resolved_prefab;
            if (!body_mesh.empty())
            {
                resolved_prefab = std::move(body_mesh);
            }
            else if (is_prefab_icon)
            {
                resolved_prefab = std::string(icon_prefab); // the `_Prefab_` icon already names the real mesh
            }
            else
            {
                // The icon is not a mesh, so the stripped icon_prefab is a bogus item name and the item drops out as
                // UI-only even though a mesh exists. Recover it from the variant entry list - held / non-body items
                // (a lantern) keep their mesh in an untoken default entry there even when the rule chain is empty.
                // Fall back to the icon name only when the item has no variant mesh.
                resolved_prefab = variant_meshes.empty() ? std::string(icon_prefab) : variant_meshes.front();
            }
            // Prefer the LIVE partprefab wrapper name. For helm variants the engine instantiates `<mesh>_dd`. The bare
            // `<mesh>` has no live wrapper, so the icon/rule-derived name never matches the swap/carrier path. Rewrite
            // to `_dd` only when the bare form is NOT itself a live wrapper but its `_dd` twin IS, so ordinary items
            // (whose bare name is the live wrapper) stay untouched. See the carrier_defaults.hpp Oongka-helm note.
            if (live_wrapper_set.count(resolved_prefab) == 0 && live_wrapper_set.count(resolved_prefab + "_dd") != 0)
                resolved_prefab += "_dd";
            for (auto &vm : variant_meshes)
            {
                if (live_wrapper_set.count(vm) == 0 && live_wrapper_set.count(vm + "_dd") != 0)
                    vm += "_dd";
            }
            ItemEntry e;
            e.runtime_idx = id;
            e.internal_name = std::move(internal_name);
            e.icon_slot = slot;
            e.full_icon = full;
            e.icon_prefab = std::move(resolved_prefab);
            e.base = extract_base_prefix(e.icon_prefab);
            e.variant_meshes = std::move(variant_meshes);
            items.push_back(std::move(e));
        }
        logger.info("[itemprefab] item-pass: {} entries collected, {} skipped", items.size(), skipped);

        // Targeted phantom recovery. Find each item whose icon_prefab and base are not in the pool yet, then scan
        // memory once for those exact strings through a first-4-byte hash table. SKIP an item whose FullIconString
        // lacks the `_Prefab_` infix. Those are UI-only icons, and without the skip the targeted search resolves
        // quest/abyssgear strings against unrelated quest-registry entries that share the post-strip name and are not
        // actual mesh prefabs.
        {
            std::vector<std::string> phantom_targets;
            for (const auto &e : items)
            {
                if (e.icon_prefab.empty())
                    continue;
                if (pool_set.find(e.icon_prefab) != pool_set.end())
                    continue;
                const std::string full_lower = to_lower(e.full_icon);
                if (is_ui_only_icon(full_lower))
                    continue;
                phantom_targets.push_back(e.icon_prefab);
            }
            std::sort(phantom_targets.begin(), phantom_targets.end());
            phantom_targets.erase(std::unique(phantom_targets.begin(), phantom_targets.end()), phantom_targets.end());
            const auto t_t_0 = std::chrono::steady_clock::now();
            const auto recovered = recover_phantoms_targeted(pool_set, phantom_targets);
            const auto t_t_1 = std::chrono::steady_clock::now();
            logger.info(
                "[itemprefab] targeted recovery: {} phantom strings searched, {} found in memory ({} ms)",
                phantom_targets.size(),
                recovered,
                std::chrono::duration_cast<std::chrono::milliseconds>(t_t_1 - t_t_0).count()
            );
        }

        // A pooled name sourced from a cached or loader-registry string can carry a stray control character (e.g. an
        // embedded CR/LF in an asset name). The byte-scan and stringinfo passes already reject such bytes, but the
        // cached passes do not, so clean every entry through one chokepoint here. A re-insert into a set strips the
        // control chars and also collapses the dirty variant into its clean twin, so the writer cannot emit a split
        // row or a duplicate key for the same prefab.
        std::set<std::string> clean_pool;
        for (const auto &name : pool_set)
            clean_pool.insert(tsv_sanitize(name));
        std::vector<std::string> pool(clean_pool.begin(), clean_pool.end());

        // Drop dead bare data-name prefabs superseded by a live `_dd` wrapper (helm variants). The bare `..._index01`
        // is a string/data entry with no live partprefab wrapper, so the swap path can never match it, and the item
        // walk already linked its items to the `_dd` row. A standalone prefab row for it is therefore misleading. The
        // guard is narrow: it removes only a name whose `_dd` twin is a live wrapper and that is not itself a live
        // wrapper.
        {
            const auto before = pool.size();
            pool.erase(
                std::remove_if(
                    pool.begin(),
                    pool.end(),
                    [&](const std::string &n)
                    { return live_wrapper_set.count(n) == 0 && live_wrapper_set.count(n + "_dd") != 0; }
                ),
                pool.end()
            );
            if (pool.size() != before)
                logger.info(
                    "[itemprefab] dropped {} dead bare prefab name(s) superseded by `_dd` live wrappers",
                    before - pool.size()
                );
        }

        // Index for prefab-centric emission. exact_map maps a pool prefab to the item index whose icon_prefab equals
        // it, and base_map maps a base to every item index that shares it.
        std::unordered_map<std::string, size_t> exact_map;
        exact_map.reserve(items.size());
        std::unordered_map<std::string, std::vector<size_t>> base_map;
        base_map.reserve(items.size());
        // Claim every item's primary (icon-derived) prefab FIRST, so another item's body variant can never steal a
        // primary link in the loop below.
        for (size_t i = 0; i < items.size(); ++i)
        {
            exact_map.emplace(items[i].icon_prefab, i);
            base_map[items[i].base].push_back(i);
        }
        // Then link each item's body-mesh variants (human male / orc / female / NPC) from the variant entry list so
        // their prefab rows resolve to this item rather than orphan: the shared, rig-stripped icon can name only one.
        // emplace keeps the first claimant, so nothing overwrites a primary link, and a mesh that several items share
        // groups under its first owner.
        uint32_t variant_links = 0;
        for (size_t i = 0; i < items.size(); ++i)
        {
            for (const auto &vm : items[i].variant_meshes)
            {
                if (vm.empty() || vm == items[i].icon_prefab)
                    continue;
                if (exact_map.emplace(vm, i).second)
                {
                    base_map[extract_base_prefix(vm)].push_back(i);
                    ++variant_links;
                }
            }
        }
        logger.info("[itemprefab] body-mesh variants linked from the variant entry list: {}", variant_links);

        // Emit one row per pool prefab, plus one row for each item whose icon_prefab is NOT in the pool, so the dump
        // never silently drops an item. `items_covered` records which items an emitted row already mentions, and the
        // phantom loop after this one emits the rest.
        // get_runtime_directory() falls back to the working directory and finally to a relative
        // anchor, so there is no unavailable case left to guard.
        const std::filesystem::path path = std::filesystem::path{DMK::filesystem::get_runtime_directory()} /
                                           "CrimsonDesertLiveTransmog_itemprefabs.tsv";
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            logger.warning("[itemprefab] failed to open output file: {}", to_utf8(path));
            return;
        }

        out << "Prefab\tBase\tExactItemId\tExactItemName\tExactIconSlot\t"
               "SiblingItemIds\tSiblingItemNames\tFullIconString\tOrphan\n";

        std::set<size_t> items_covered; // items mentioned in any emitted row

        auto emit_row = [&](std::string_view prefab,
                            std::string_view row_base,
                            const ItemEntry *exact,
                            const std::vector<size_t> &siblings,
                            bool orphan)
        {
            out << prefab << '\t' << row_base << '\t';
            if (exact)
            {
                out << exact->runtime_idx << '\t' << exact->internal_name << '\t' << exact->icon_slot;
            }
            else
            {
                out << "\t\t";
            }
            out << '\t';
            for (size_t i = 0; i < siblings.size(); ++i)
            {
                if (i > 0)
                    out << ',';
                out << items[siblings[i]].runtime_idx;
            }
            out << '\t';
            for (size_t i = 0; i < siblings.size(); ++i)
            {
                if (i > 0)
                    out << ',';
                out << items[siblings[i]].internal_name;
            }
            out << '\t';
            if (exact)
                out << exact->full_icon;
            out << '\t' << (orphan ? "yes" : "no") << '\n';
        };

        uint32_t rows_emitted = 0;
        uint32_t rows_exact = 0;
        uint32_t rows_sibling_only = 0;
        uint32_t rows_orphan = 0;
        uint32_t rows_junk_skipped = 0;
        for (const auto &p : pool)
        {
            const std::string row_base = extract_base_prefix(p);
            const ItemEntry *exact = nullptr;
            if (auto it = exact_map.find(p); it != exact_map.end())
            {
                exact = &items[it->second];
                items_covered.insert(it->second);
            }
            std::vector<size_t> siblings;
            if (auto it = base_map.find(row_base); it != base_map.end())
            {
                for (size_t idx : it->second)
                {
                    if (exact && &items[idx] == exact)
                        continue;
                    siblings.push_back(idx);
                    items_covered.insert(idx);
                }
            }
            const bool orphan = !exact && siblings.empty();
            // Skip a junk exe-static string fragment (cd_<rig>_<digits><letters>) that resolves to no item - it is not
            // a real mesh, just noise the cd_-prefix pool walk captured. Guarded on `orphan`, so a linked prefab is
            // never dropped even if one somehow matched the shape.
            if (orphan && is_junk_rig_fragment(p))
            {
                ++rows_junk_skipped;
                continue;
            }
            if (exact)
                ++rows_exact;
            else if (!siblings.empty())
                ++rows_sibling_only;
            else
                ++rows_orphan;
            emit_row(p, row_base, exact, siblings, orphan);
            ++rows_emitted;
        }

        // Phantom items - their icon_prefab never appeared in the pool, because the engine's stringinfo references a
        // prefab that the asset-bundle walk missed. Emit ONE row per unique phantom icon_prefab with all sharing items
        // grouped (first as exact, rest as siblings). This dump is PREFAB-based: an item only appears when it resolves
        // to a real mesh/world-object prefab. Mesh-less UI icons (quest/skill/stat/bespoke `ItemIcon_<name>` with no
        // asset-prefix mesh) are skipped. gimmick/collection/craft/puzzle ARE real prefabs and are kept (see
        // is_ui_only_icon).
        uint32_t phantom_rows = 0;
        uint32_t ui_skipped = 0;
        std::set<std::string> phantom_icons_emitted;
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (items_covered.count(i))
                continue;
            const std::string full_lower = to_lower(items[i].full_icon);
            if (is_ui_only_icon(full_lower))
            {
                // Not a real mesh prefab (UI-only icon), so skip it. This dump is prefab-based.
                ++ui_skipped;
                continue;
            }
            if (!phantom_icons_emitted.insert(items[i].icon_prefab).second)
                continue;
            // Collect every other item that shares this icon_prefab via base_map. extract_base_prefix returns the
            // icon_prefab itself when there is no `_NNNN` token, so the base lookup is the correct grouping key.
            std::vector<size_t> sharing;
            if (auto it = base_map.find(items[i].base); it != base_map.end())
            {
                for (size_t idx : it->second)
                {
                    if (idx == i)
                        continue;
                    if (items[idx].icon_prefab != items[i].icon_prefab)
                        continue;
                    sharing.push_back(idx);
                    items_covered.insert(idx);
                }
            }
            emit_row(items[i].icon_prefab, items[i].base, &items[i], sharing, false);
            ++phantom_rows;
            ++rows_emitted;
        }

        logger.info(
            "[itemprefab] dumped {} rows: {} exact, {} sibling-only, "
            "{} orphan, {} phantom-item, {} UI-only skipped, {} junk-fragment skipped -> "
            "CrimsonDesertLiveTransmog_itemprefabs.tsv",
            rows_emitted,
            rows_exact,
            rows_sibling_only,
            rows_orphan,
            phantom_rows,
            ui_skipped,
            rows_junk_skipped
        );
    }
    namespace
    {
        // Owns the dump thread. Both callers (init-time and the apply worker's post-scan) share it, so a second
        // request while one is running is a no-op rather than a duplicate walk over the same registries.
        std::optional<DMK::StoppableWorker> s_dump_worker;
        std::mutex s_dump_worker_mutex;
    } // namespace

    void launch_itemmesh_dump()
    {
        std::lock_guard<std::mutex> lk(s_dump_worker_mutex);
        if (s_dump_worker.has_value() && s_dump_worker->is_running())
        {
            return;
        }
        // Retire a finished worker before replacing it: the destructor joins, and this one has already exited.
        s_dump_worker.reset();
        try
        {
            s_dump_worker.emplace("LtItemMeshDump", [](std::stop_token stop) { dump_itemmesh_tsv(stop); });
        }
        catch (const std::exception &e)
        {
            DMK::log().warning("[itemprefab] could not start the dump worker: {}", e.what());
        }
    }

    void join_itemmesh_dump() noexcept
    {
        std::lock_guard<std::mutex> lk(s_dump_worker_mutex);
        // ~StoppableWorker requests stop and joins, so the reset IS the join.
        s_dump_worker.reset();
    }

} // namespace Transmog
