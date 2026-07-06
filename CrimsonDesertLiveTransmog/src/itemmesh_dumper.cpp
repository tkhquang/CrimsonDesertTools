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
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Transmog
{
    namespace
    {
        // Registry-holder absolute addresses are resolved on first call via the AOB cascade defined in
        // `aob_resolver.hpp` (k_iteminfoHolderCandidates / k_stringinfoHolderCandidates) and cached for the lifetime of
        // the process. Both holders dereference to a registry struct with the standard `+0x08 = u32 count`, `+0x50 =
        // QWORD entry-array` layout.
        constexpr ptrdiff_t kOffRegistryCount = 0x08;
        constexpr ptrdiff_t kOffRegistryArray = 0x50;

        constexpr ptrdiff_t kOffName = 0x08;
        constexpr ptrdiff_t kOffMetaSub = 0x90;

        // The engine stores strings behind a "StringRef" descriptor:
        // {char* ptr @+0x00, u32 len @+0x08}. The characters are always heap-allocated (confirmed down to a 5-character
        // string), so there is no inline/SSO form to special-case.
        //
        // Two wrapper shapes reference a StringRef:
        //   - Item NAME wrappers (desc+0x08) ARE a StringRef directly.
        //   - stringinfo ICON wrappers are a larger object that stores a pointer to their StringRef at +0x18. For an
        //     inline icon wrapper that pointer targets the wrapper's own +0x20; for an external one it targets a
        //     StringRef elsewhere on the heap. Dereferencing +0x18 resolves both forms, whereas a fixed +0x20/+0x28
        //     read only handles the inline form and reads garbage on the external one.
        constexpr ptrdiff_t kOffWrapDesc = 0x18; // icon wrapper -> StringRef*
        constexpr ptrdiff_t kOffRefPtr = 0x00;   // StringRef: char*
        constexpr ptrdiff_t kOffRefLen = 0x08;   // StringRef: u32 length

        // Item body-mesh chain. An item carries its actual per-rig mesh prefab names here, independent of the icon.
        // Several rig variants of one item share a single icon -- Daeil_Band, Damian_Daeil_Band and OOngka_Daeil_Band
        // all expose ItemIcon_Prefab_cd_phm_... yet emit cd_phm / cd_phw / cd_pom meshes respectively -- so the icon
        // alone collapses them onto the cd_phm prefab. desc+0x248 holds a rule-list pointer (count at +0x250); each
        // rule's mesh-slot array (u16 stringinfo slots) is at rule+0x00 with its count at rule+0x08.
        constexpr ptrdiff_t kOffDescRuleList = 0x248;
        constexpr ptrdiff_t kOffDescRuleCount = 0x250;
        constexpr ptrdiff_t kRuleStride = 0x38;
        constexpr ptrdiff_t kOffRuleMeshArr = 0x00;
        constexpr ptrdiff_t kOffRuleMeshCount = 0x08;
        constexpr uint32_t kRuleScanCap = 64; // bound on garbage counts

        // Per-body mesh variant entry list (desc+0x3E0), the structure the engine's variant resolver walks to
        // render an item's mesh for the wearer's body. Each 0x58-byte entry names one body variant of the item;
        // the mesh handle is reached via a pointer at entry+0x10 (deref -> handle whose low 16 bits index stringinfo).
        // This is the authoritative, per-item, noise-free source of every mesh an item uses across bodies (human male /
        // orc / female / NPC), which the shared, rig-stripped icon cannot enumerate -- e.g. Kliff_Mask binds three rig
        // meshes here (`cd_phm_` / `cd_pom_` / `cd_phw_`) behind one `cd_phm_` icon. 1.13 offsets; re-confirm on a
        // descriptor reshape. (The engine also stores each entry's wearer body-class token array at entry+0x40 / count
        // +0x48; the dump links every entry's mesh regardless of which body selects it, so those are not needed here.)
        constexpr ptrdiff_t kOffDescVariantEntries = 0x3E0;
        constexpr ptrdiff_t kOffDescVariantCount = 0x3E8;
        constexpr ptrdiff_t kVariantEntryStride = 0x58;
        constexpr ptrdiff_t kOffEntryMesh = 0x10; // pointer into the variant table -> mesh handle
        constexpr uint32_t kVariantEntryCap = 32;

        // ----- safe reads -----
        //
        // The `(value, bool& ok)` shape distinguishes a faulted read from a legitimate zero result, which matters at
        // call sites where 0 is a valid value (e.g. a slot index of 0). `DMKMemory::seh_read` is the underlying
        // SEH-protected primitive; these adapters fold its `std::optional<T>` return into the local shape used by the
        // dumper's bounded walks.

        uintptr_t read_qword_safe(uintptr_t addr, bool &ok) noexcept
        {
            const auto v = DMKMemory::seh_read<uintptr_t>(addr);
            ok = v.has_value();
            return v.value_or(0);
        }

        uint32_t read_u32_safe(uintptr_t addr, bool &ok) noexcept
        {
            const auto v = DMKMemory::seh_read<uint32_t>(addr);
            ok = v.has_value();
            return v.value_or(0);
        }

        uint16_t read_u16_safe(uintptr_t addr, bool &ok) noexcept
        {
            const auto v = DMKMemory::seh_read<uint16_t>(addr);
            ok = v.has_value();
            return v.value_or(0);
        }

        size_t read_printable_into(const char *src, char *dst, size_t maxLen) noexcept
        {
            size_t n = 0;
            __try
            {
                for (size_t i = 0; i < maxLen; ++i)
                {
                    char c = src[i];
                    if (c == 0)
                        break;
                    // Reject control bytes (0x01..0x1F): a misaligned or torn heap read lands on pointer/length bytes
                    // that carry low control values, so a control byte aborts the read. High bytes (0x80..0xFF) are
                    // accepted because some item name keys are UTF-8 encoded -- the
                    // Roman numerals in `Goblin_Merchant_Fabric_Armor_*` use `E2 85 A2..A5` -- and a
                    // printable-ASCII-only filter would zero the whole name, dropping the item from the prefab pass
                    // entirely (it never reaches the exact/base maps, so its prefab loses its item link). Mirrors the
                    // name reader in item_name_table.cpp.
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

        std::string read_cstr_safe(const char *src, size_t maxLen) noexcept
        {
            char buf[512];
            const size_t cap = maxLen < sizeof(buf) ? maxLen : sizeof(buf);
            size_t n = read_printable_into(src, buf, cap);
            return std::string(buf, n);
        }

        // Read a StringRef descriptor {char* ptr @+0x00, u32 len @+0x08}.
        std::string read_string_ref(uintptr_t node) noexcept
        {
            if (!node)
                return {};
            bool ok = false;
            const uint32_t len = read_u32_safe(node + kOffRefLen, ok);
            if (!ok || len == 0 || len > 0x10000)
                return {};
            const uintptr_t cstr = read_qword_safe(node + kOffRefPtr, ok);
            if (!ok || cstr < 0x10000)
                return {};
            return read_cstr_safe(reinterpret_cast<const char *>(cstr), len + 1);
        }

        // stringinfo ICON wrapper: the StringRef is reached via wrap+0x18 (inline wrappers point back at their own
        // +0x20; external ones point elsewhere). Dereference it so both layouts resolve.
        std::string read_wrapper_string(uintptr_t wrap) noexcept
        {
            if (!wrap)
                return {};
            bool ok = false;
            const uintptr_t node = read_qword_safe(wrap + kOffWrapDesc, ok);
            if (!ok)
                return {};
            return read_string_ref(node);
        }

        // Distinguish icon strings that map to a real mesh prefab from those that are UI-texture-only. The engine
        // writes `ItemIcon_Prefab_<name>` for icons backed by a `.prefab` file (the rows the dumper wants to emit) and
        // `ItemIcon_<name>` (no `_Prefab_` infix) for icons that resolve to a UI texture bundle (quest dialogs,
        // abyss-gear sockets, money/trade UI, collection-album thumbnails, housing-UI icons, puzzle slots, and so on).
        // Two cd_* sub-families (`cd_questimage_*` and `cd_knowledgeimage_*`) are likewise UI-only and must be filtered
        // before the targeted phantom-recovery pass, otherwise it would resolve unrelated quest-registry strings
        // against mesh-prefab names that happen to overlap post-strip.
        bool starts_with_asset_prefix(std::string_view sl) noexcept; // fwd decl

        // True when the icon string does NOT encode a real mesh/world-object prefab -- i.e. it is UI-texture-only
        // (quest dialogs, abyss-gear, skill/stat items, money/trade UI, memory chips, dev stubs, or a bespoke
        // `ItemIcon_<ItemName>` whose post-strip name is not a known asset family).
        //
        // An icon encodes a mesh when, after stripping the `itemicon_`(`prefab_`) wrapper, the remainder begins with a
        // known asset-prefix family (the same accept-list the pool enrich pass uses:
        // cd_/gimmick_/collection_/craft_/puzzle_/background_/lamp_/fs_/ docking_/item_rare_). Classifying on the
        // family (not on the `itemicon_prefab_` infix alone) is deliberate: gimmick/collection/ craft/puzzle
        // world-objects own real meshes but reference them through the bare `itemicon_` form, so they must still count
        // as prefabs.
        bool is_ui_only_icon(std::string_view fullIconLower) noexcept
        {
            // `cd_questimage_*` / `cd_knowledgeimage_*` are UI-texture bundle entries with no underlying mesh.
            constexpr std::string_view kCdUi[] = {
                "cd_questimage_",
                "cd_knowledgeimage_",
            };
            for (auto p : kCdUi)
            {
                if (fullIconLower.size() >= p.size() && fullIconLower.substr(0, p.size()) == p)
                    return true;
            }
            // `itemicon_prefab_<cd_mesh>` is always a real mesh.
            constexpr std::string_view kItemiconMesh = "itemicon_prefab_";
            constexpr std::string_view kItemiconAll = "itemicon_";
            std::string_view rest = fullIconLower;
            if (rest.size() >= kItemiconMesh.size() && rest.substr(0, kItemiconMesh.size()) == kItemiconMesh)
                return false;
            if (rest.size() >= kItemiconAll.size() && rest.substr(0, kItemiconAll.size()) == kItemiconAll)
                rest = rest.substr(kItemiconAll.size());
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

        // PASS 1a: build the pool from the prefab_wrapper_swap cached catalog. Engine-validated, zero garbage, but
        // vtable-filtered (`0x145BC4638` inside walk_string_info) -- drops some legit cd_* prefabs (bags, monsters,
        // NPCs, accessories). The supplementary stringinfo walk in PASS 1b recovers those.
        std::size_t build_pool_from_catalog(std::set<std::string> &out)
        {
            auto &logger = DMK::Logger::get_instance();
            if (!PrefabWrapperSwap::is_catalog_populated())
            {
                logger.warning("[itemprefab] prefab catalog not yet populated");
                return 0;
            }
            const auto &cat = PrefabWrapperSwap::slot_catalog(Transmog::TransmogSlot::Helm);
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
        // k_helmSlotId "helm is the only pair with a suffix").
        std::size_t collect_live_wrapper_names(std::set<std::string> &out)
        {
            if (!PrefabWrapperSwap::is_catalog_populated())
                return 0;
            std::size_t added = 0;
            for (std::size_t s = 0; s < static_cast<std::size_t>(Transmog::TransmogSlot::Count); ++s)
            {
                const auto &cat = PrefabWrapperSwap::slot_catalog(static_cast<Transmog::TransmogSlot>(s));
                for (const auto &e : cat)
                {
                    if (!e.name.empty() && out.insert(to_lower(e.name)).second)
                        ++added;
                }
            }
            return added;
        }

        // Allow-list of asset-name prefixes for the stringinfo enrich pass. Each accepted family resolves to a real
        // `.prefab` file on disk; UI-only families (`itemicon_*`, `cd_questimage_*`, `cd_knowledgeimage_*`) are
        // rejected first so the pool never contains texture-bundle entries that lack an underlying mesh.
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
            // Accept-list (each ends with underscore so we don't pick up name collisions like `cdkey_*` or
            // `gimmickspecial_*`).
            constexpr std::string_view kAccepts[] = {"cd_",     "gimmick_",    "collection_", "craft_",
                                                     "puzzle_", "background_", "lamp_",       "fs_phm_",
                                                     "fs_phw_", "docking_",    "item_rare_"};
            for (auto p : kAccepts)
            {
                if (sl.size() >= p.size() && sl.substr(0, p.size()) == p)
                    return true;
            }
            return false;
        }

        // PASS 1b: supplementary stringinfo walk without the vtable filter. Picks up the asset families the
        // swap-catalog's vtable gate drops AND the non-cd_ families (gimmick, collection, craft, puzzle, etc.) that
        // prefab_wrapper_swap doesn't index at all. Only enriches the dumper's pool; prefab_wrapper_swap is untouched
        // so live swap path + UI picker keep their scope.
        std::size_t enrich_pool_from_stringinfo(std::set<std::string> &out, uintptr_t stringArr, uint32_t stringCount)
        {
            bool ok = false;
            std::size_t added = 0;
            for (uint32_t i = 0; i < stringCount; ++i)
            {
                const uintptr_t wrap = read_qword_safe(stringArr + i * 8ull, ok);
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
        // heap-loaded asset strings (gimmick/collection/cd_ex_*/etc.) cluster in the low 0x01-0x20 VA band, so scanning
        // just this window is fast and catches the bulk of them; the wider full-VA walk in recover_phantoms_targeted
        // picks up the families that land outside it.
        constexpr uintptr_t kAssetScanStart = 0x01000000;
        constexpr uintptr_t kAssetScanEnd = 0x20000000;

        size_t safe_memcpy(char *dst, const void *src, size_t n) noexcept
        {
            __try
            {
                std::memcpy(dst, src, n);
                return n;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        bool is_prefab_char(char c) noexcept
        {
            unsigned char u = static_cast<unsigned char>(c);
            return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_';
        }

        // True at a position that looks like the start of an asset-name string we want. Matches all the prefixes the
        // stringinfo enrich accepts (cd_/gimmick_/collection_/craft_/puzzle_/background_/
        // lamp_/fs_/docking_/item_rare_). Case-insensitive on the first letter; later chars use the same set as
        // `is_prefab_char` plus a fast-fail.
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
            // gimmick must be matched first since avail<7 returned; craft_ (6 chars) and lamp_ (5 chars) are shorter,
            // so each is checked below under its own length guard:
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

        // PASS 1e: targeted phantom recovery. After PASS 1a-1d build the pool, items whose iconPrefab isn't in pool
        // become "phantom candidates". This pass walks memory once searching for the exact phantom strings via a
        // first-4-byte hash table -- O(memory_size + N_phantoms) total. Catches byte-scan misses where the asset string
        // lives in a region the chunked walker mis-handled or where the prefix gate didn't match.
        std::size_t recover_phantoms_targeted(std::set<std::string> &pool, const std::vector<std::string> &phantomNames)
        {
            if (phantomNames.empty())
                return 0;

            // Index phantoms by first 4 bytes (lowercased).
            std::unordered_map<uint32_t, std::vector<size_t>> byFirst4;
            byFirst4.reserve(phantomNames.size() * 2);
            for (size_t i = 0; i < phantomNames.size(); ++i)
            {
                if (phantomNames[i].size() < 4)
                    continue;
                uint32_t k = 0;
                std::memcpy(&k, phantomNames[i].data(), 4);
                byFirst4[k].push_back(i);
            }
            if (byFirst4.empty())
                return 0;

            std::vector<uint8_t> found(phantomNames.size(), 0);
            std::size_t recovered = 0;

            // Targeted recovery walks the FULL user address range because asset string heap addresses shift between
            // reloads (low 0x02xxxxx OR high 0xEFxxxxxx depending on allocation order). The per-byte cost is one uint32
            // hash lookup -- fast enough to cover the whole VA in <10s.
            constexpr size_t kChunk = 0x100000;
            std::vector<char> scratch(kChunk);
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            const uintptr_t addrEnd = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
            uintptr_t addr = 0x10000;
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
                    for (size_t off = 0; off < total; off += kChunk)
                    {
                        const size_t want = std::min<size_t>(kChunk, total - off);
                        const size_t got = safe_memcpy(scratch.data(), reinterpret_cast<const void *>(bs + off), want);
                        if (got < 4)
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
                            auto it = byFirst4.find(key);
                            if (it == byFirst4.end())
                                continue;
                            for (size_t pi : it->second)
                            {
                                if (found[pi])
                                    continue;
                                const auto &target = phantomNames[pi];
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
            constexpr size_t kChunk = 0x100000;
            std::vector<char> scratch(kChunk);
            uintptr_t addr = kAssetScanStart;
            while (addr < kAssetScanEnd)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
                    break;
                const DWORD readable =
                    PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY;
                // Scan any committed readable region within the bounded VA window (kAssetScanStart..kAssetScanEnd).
                // MEM_PRIVATE is welcome here because Crimson Desert's heap-loaded asset strings
                // (gimmick/collection/`cd_ex_*`/etc.) live in mid-size MEM_PRIVATE regions clustered in the 0x01-0x20
                // VA band. PAGE_GUARD pages would AV, skip. MEM_IMAGE is included -- harmless since asset names rarely
                // appear in .rdata literals.
                if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) != 0 && (mbi.Protect & PAGE_GUARD) == 0)
                {
                    const uintptr_t bs = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                    const size_t total = static_cast<size_t>(mbi.RegionSize);
                    for (size_t off = 0; off < total; off += kChunk)
                    {
                        const size_t want = std::min<size_t>(kChunk, total - off);
                        const size_t got = safe_memcpy(scratch.data(), reinterpret_cast<const void *>(bs + off), want);
                        if (got > 0)
                            scan_chunk_for_asset_strings(scratch.data(), got, out);
                    }
                }
                const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (next <= addr)
                    break;
                addr = next;
            }
            return out.size() - before;
        }

        // TSV columns are tab-delimited and rows newline-delimited, so a control character inside a name (a cached
        // asset string can carry an embedded CR/LF) would split or shift the row when written. Strip TAB/CR/LF so the
        // name can neither corrupt a row nor differ from its clean twin once both are pooled.
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

        // Resolve the item's primary body-mesh prefab from its rule chain. With a non-empty @p iconStem, returns the
        // mesh whose rig-stripped name equals it -- the same logical item in its actual wearer rig -- which lets rig
        // variants link to cd_phw / cd_pom instead of the shared cd_phm icon. Sub-part meshes (e.g. `..._wire_0001_r`)
        // have a different stem and are skipped; for an ordinary item the matching mesh equals the icon prefab, so the
        // result is identical and only genuine rig variants change.
        //
        // When @p iconStem is empty -- the item's icon string is missing or a bare `ItemIcon_` stub, so there is
        // nothing to match against -- fall back to the first asset-prefab mesh named in the rule chain. The item still
        // owns a real mesh there, so this links it to its prefab instead of leaving that prefab orphaned. Returns ""
        // only when no usable mesh is found, so the caller keeps the icon-derived prefab.
        std::string resolve_rule_body_mesh(uintptr_t desc, uintptr_t stringArr, uint32_t stringCount,
                                           std::string_view iconStem) noexcept
        {
            if (!desc || !stringArr)
                return {};
            bool ok = false;
            const uintptr_t ruleList = read_qword_safe(desc + kOffDescRuleList, ok);
            const uint32_t ruleCount = read_u32_safe(desc + kOffDescRuleCount, ok);
            if (!ruleList || ruleCount == 0 || ruleCount > kRuleScanCap)
                return {};
            std::string firstBodyMesh;
            for (uint32_t r = 0; r < ruleCount; ++r)
            {
                const uintptr_t rule = ruleList + static_cast<uintptr_t>(r) * kRuleStride;
                const uintptr_t meshArr = read_qword_safe(rule + kOffRuleMeshArr, ok);
                const uint32_t meshCount = read_u32_safe(rule + kOffRuleMeshCount, ok);
                if (!meshArr || meshCount == 0 || meshCount > kRuleScanCap)
                    continue;
                for (uint32_t m = 0; m < meshCount; ++m)
                {
                    const uint16_t mslot = read_u16_safe(meshArr + static_cast<uintptr_t>(m) * 2, ok);
                    if (!ok || mslot == 0xFFFF || mslot >= stringCount)
                        continue;
                    const uintptr_t wrap = read_qword_safe(stringArr + static_cast<uintptr_t>(mslot) * 8, ok);
                    std::string mesh = to_lower(read_wrapper_string(wrap));
                    if (mesh.empty())
                        continue;
                    if (!iconStem.empty())
                    {
                        if (strip_rig_prefix(mesh) == iconStem)
                            return mesh;
                    }
                    else if (firstBodyMesh.empty() && starts_with_asset_prefix(mesh))
                    {
                        firstBodyMesh = std::move(mesh);
                    }
                }
            }
            return firstBodyMesh;
        }

        // ----- per-item record collected from iteminfo/stringinfo -----

        // A garbage byte fragment the "cd_"-prefix pool walk picks up: a body-rig prefix (cd_phm_ / cd_pom_ / ... which
        // always name a body PART) directly followed by a short digits-then-letters token and nothing else -- e.g.
        // `cd_pom_0jl`, `cd_phm_00fv` (proven in memory to be a `cd_pom_0jl,<binary>` exe-static fragment). A real
        // body-rig mesh carries a `_<NN>_<part>_` or `_m<NNNN>_` structure, so it never has this shape, and none of
        // these fragments is ever linked to an item. Used to skip them at emission so the dump stays clean.
        bool is_junk_rig_fragment(std::string_view name) noexcept
        {
            static constexpr std::string_view kRigs[] = {"cd_phm_", "cd_phw_", "cd_pom_", "cd_pgm_",
                                                         "cd_pdm_", "cd_ptm_", "cd_nhm_", "cd_ndm_"};
            std::string_view tail;
            for (auto rig : kRigs)
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

        // Resolve EVERY distinct mesh an item uses across bodies, from the per-body variant entry list (desc+0x3E0) --
        // the authoritative linkage source. Each entry's mesh (via entry+kOffEntryMesh -> handle -> stringinfo slot) is
        // one body variant of the item (human male / orc / female / NPC), all item-specific and noise-free; the shared,
        // rig-stripped icon cannot enumerate them. The ground-drop mesh (`..._dropitem_...`) is skipped -- it is a
        // shared prop, not a wearer variant. Returns the deduplicated meshes in entry order (the first is the item's
        // default/primary mesh); empty for items with no variant list, which fall back to their icon-derived prefab.
        std::vector<std::string> resolve_variant_meshes(uintptr_t desc, uintptr_t stringArr,
                                                        uint32_t stringCount) noexcept
        {
            std::vector<std::string> meshes;
            if (!desc || !stringArr)
                return meshes;
            bool ok = false;
            const uintptr_t entries = read_qword_safe(desc + kOffDescVariantEntries, ok);
            const uint32_t count = read_u32_safe(desc + kOffDescVariantCount, ok);
            if (!entries || count == 0 || count > kVariantEntryCap)
                return meshes;
            for (uint32_t e = 0; e < count; ++e)
            {
                const uintptr_t entry = entries + static_cast<uintptr_t>(e) * kVariantEntryStride;
                // entry+kOffEntryMesh points into the variant table at the entry's mesh handle; deref -> handle whose
                // low 16 bits index stringinfo.
                const uintptr_t meshPtr = read_qword_safe(entry + kOffEntryMesh, ok);
                if (!meshPtr)
                    continue;
                const uintptr_t handle = read_qword_safe(meshPtr, ok);
                if (!ok)
                    continue;
                const uint16_t slot = static_cast<uint16_t>(handle & 0xFFFFu);
                if (slot == 0xFFFF || slot >= stringCount)
                    continue;
                const uintptr_t wrap = read_qword_safe(stringArr + static_cast<uintptr_t>(slot) * 8, ok);
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
            uint32_t runtimeIdx;
            std::string internalName;
            uint16_t iconSlot;
            std::string fullIcon;    // raw stringinfo string, may have ItemIcon_Prefab_ prefix
            std::string iconPrefab;  // lowercased, prefix stripped
            std::string base;        // last `_NNNN`-anchored prefix of iconPrefab
            // Distinct player rig meshes (Kliff / Oongka / Damiane) recovered from the variant entry list; empty when
            // the item has none. Links the male / orc / female body variants the icon-derived prefab cannot pair.
            std::vector<std::string> variantMeshes;
        };
    } // namespace

    void dump_itemmesh_tsv()
    {
        auto &logger = DMK::Logger::get_instance();

        // Gate on the swap catalog before sourcing the prefab pool. PASS 1a reads PrefabWrapperSwap's slot catalog,
        // which populate_slot_catalogs() builds once a world is loaded (on the save-load tick, or lazily on first
        // overlay open). Both dump triggers spawn this routine in a detached thread the instant the dump flag is set,
        // which can beat that build and leave PASS 1a with nothing -- an incomplete pool.
        //
        // Wait for the catalog rather than capping the wait: there is no valid timeout here. A player can sit at the
        // main menu for days with no world and therefore no catalog, so any cap would just skip the dump (or fall back
        // to a degraded one) for a state that is normal, not failed. Polling until the catalog exists yields a complete
        // dump whenever the world finally loads, however long that takes. The wait is free (this already runs off the
        // main thread) and the thread is reclaimed at process exit if a world is never entered.
        if (!PrefabWrapperSwap::is_catalog_populated())
        {
            logger.info("[itemprefab] waiting for swap catalog before dumping");
            constexpr DWORD k_catalogPollMs = 500;
            while (!PrefabWrapperSwap::is_catalog_populated())
                Sleep(k_catalogPollMs);
        }

        // Resolve both registry-holder absolute addresses via the AOB cascade. The patterns target the `mov reg,
        // [rip+disp32]` load of each holder in a stable lookup primitive (see aob_resolver.hpp). The cascade returns
        // the absolute address of the holder slot; one indirection yields the registry struct (header + +0x08 count +
        // +0x50 entry array).
        const uintptr_t iteminfoHolderAddr = resolve_address(k_iteminfoHolderCandidates, "IteminfoHolder");
        const uintptr_t stringinfoHolderAddr = resolve_address(k_stringinfoHolderCandidates, "StringinfoHolder");
        if (!iteminfoHolderAddr || !stringinfoHolderAddr)
        {
            logger.warning("[itemprefab] holder AOB resolve failed: "
                           "iteminfo=0x{:X} stringinfo=0x{:X}",
                           iteminfoHolderAddr, stringinfoHolderAddr);
            return;
        }

        // `ok` is reused across these reads and its intermediate values are deliberately ignored: a faulted read yields
        // 0 via value_or(0), which the explicit null/zero checks below already reject.
        bool ok = false;
        const uintptr_t iteminfoMgr = read_qword_safe(iteminfoHolderAddr, ok);
        const uintptr_t stringinfoMgr = read_qword_safe(stringinfoHolderAddr, ok);
        if (!iteminfoMgr || !stringinfoMgr)
        {
            logger.warning("[itemprefab] registry holder null: iteminfo=0x{:X} stringinfo=0x{:X}", iteminfoMgr,
                           stringinfoMgr);
            return;
        }
        const uint32_t itemCount = read_u32_safe(iteminfoMgr + kOffRegistryCount, ok);
        const uintptr_t itemArr = read_qword_safe(iteminfoMgr + kOffRegistryArray, ok);
        const uint32_t stringCount = read_u32_safe(stringinfoMgr + kOffRegistryCount, ok);
        const uintptr_t stringArr = read_qword_safe(stringinfoMgr + kOffRegistryArray, ok);
        logger.info("[itemprefab] iteminfo: count={} arr=0x{:X}  stringinfo: count={} arr=0x{:X}", itemCount, itemArr,
                    stringCount, stringArr);
        if (itemCount == 0 || !itemArr || stringCount == 0 || !stringArr)
        {
            logger.warning("[itemprefab] one of the registries is empty");
            return;
        }

        // PASS 1a -- pool from prefab_wrapper_swap cached catalog. PASS 1b -- supplementary stringinfo walk for cd_*
        // entries
        //            the swap-catalog's vtable filter dropped.
        // PASS 1c -- AppearanceTableLoader registry, unfiltered (the
        //            internal walker filters by body-mesh slot tag and
        //            drops ~7,400 entries; we want them all here for
        //            the gimmick/collection/lamp/puzzle families).
        // PASS 1d -- MEM_MAPPED asset-pool byte-scan (residual catch).
        const auto t0 = GetTickCount64();
        std::set<std::string> poolSet;
        const auto fromCatalog = build_pool_from_catalog(poolSet);
        const auto fromStringinfo = enrich_pool_from_stringinfo(poolSet, stringArr, stringCount);
        std::size_t fromLoader = 0;
        PrefabWrapperSwap::for_each_loader_prefab_name(
            [&](std::string_view name)
            {
                if (poolSet.insert(to_lower(name)).second)
                    ++fromLoader;
            });
        const auto fromAssetPool = enrich_pool_from_asset_pool(poolSet);
        const auto t1 = GetTickCount64();
        logger.info("[itemprefab] pool pre-targeted: {} catalog + {} stringinfo + "
                    "{} loader-reg + {} asset-pool = {} total ({} ms)",
                    fromCatalog, fromStringinfo, fromLoader, fromAssetPool, poolSet.size(), t1 - t0);
        if (poolSet.empty())
        {
            logger.warning("[itemprefab] prefab pool empty -- aborting dump.");
            return;
        }

        // Live partprefab wrapper names (from the swap catalogs). Consulted in PASS 2 to link items to the `_dd`
        // helm-variant wrapper the swap/carrier path actually matches, and in PASS 4 to drop the dead bare data-name.
        std::set<std::string> liveWrapperSet;
        const auto liveWrapperCount = collect_live_wrapper_names(liveWrapperSet);
        logger.info("[itemprefab] live-wrapper set: {} names (for `_dd` helm-suffix linking)", liveWrapperCount);

        // PASS 2 -- walk iteminfo, collect item entries with their icon-derived prefab + base. The TSV is emitted in
        // pass 3 from the pool's perspective, so we just collect here.
        std::vector<ItemEntry> items;
        items.reserve(itemCount);
        uint32_t skipped = 0;
        for (uint32_t id = 0; id < itemCount; ++id)
        {
            const uintptr_t desc = read_qword_safe(itemArr + id * 8ull, ok);
            if (!desc)
            {
                ++skipped;
                continue;
            }
            const uintptr_t nameWrap = read_qword_safe(desc + kOffName, ok);
            // The name wrapper IS a StringRef directly (no icon-style +0x18 indirection), so read it as one.
            std::string internalName = read_string_ref(nameWrap);
            if (internalName.empty())
            {
                // Recover entries whose StringRef length field is unreadable or zero while the character pointer is
                // still valid: read the pointer target with a fixed bound. read_cstr_safe stops at the first
                // non-printable byte, so the bound cannot overrun.
                const uintptr_t altPtr = read_qword_safe(nameWrap, ok);
                if (ok && altPtr >= 0x10000)
                    internalName = read_cstr_safe(reinterpret_cast<const char *>(altPtr), 128);
            }
            if (internalName.empty())
            {
                ++skipped;
                continue;
            }
            const uintptr_t metaSub = read_qword_safe(desc + kOffMetaSub, ok);
            if (!metaSub)
            {
                ++skipped;
                continue;
            }
            const uint16_t slot = read_u16_safe(metaSub, ok);
            if (!ok || slot == 0xFFFF || slot >= stringCount)
            {
                ++skipped;
                continue;
            }
            const uintptr_t wrap = read_qword_safe(stringArr + slot * 8ull, ok);
            const std::string full = read_wrapper_string(wrap);
            if (full.empty())
            {
                ++skipped;
                continue;
            }

            // Case-insensitive prefix strip. Icons come in three flavors:
            //   ItemIcon_Prefab_<cd_mesh>   -- body-mesh prefabs (cd_*)
            //   ItemIcon_<gimmick|collection|craft|puzzle>_<name>
            //                               -- world-object prefabs that
            //                                  share the icon namespace
            //                                  but use a different
            //                                  mesh-prefab family
            //   itemicon_quest|abyssgear_*  -- UI-only (no mesh)
            // Strip `itemicon_prefab_` first (cd_*) else strip just `itemicon_` so the remainder is the underlying mesh
            // name (e.g. `collection_prop_doll_0001`) that the pool indexes -- otherwise items resolve to themselves as
            // phantoms.
            std::string fullLower = to_lower(full);
            std::string_view iconPrefab = fullLower;
            constexpr std::string_view kLongPrefix = "itemicon_prefab_";
            constexpr std::string_view kShortPrefix = "itemicon_";
            // `itemicon_prefab_<mesh>` carries the real mesh name; a bare `itemicon_<name>`
            // (e.g. `ItemIcon_Lantern_On`) does not, so its stripped remainder is a non-mesh item name that
            // must not be trusted as a prefab.
            bool isPrefabIcon = false;
            if (iconPrefab.size() >= kLongPrefix.size() && iconPrefab.substr(0, kLongPrefix.size()) == kLongPrefix)
            {
                iconPrefab = iconPrefab.substr(kLongPrefix.size());
                isPrefabIcon = true;
            }
            else if (iconPrefab.size() >= kShortPrefix.size() &&
                     iconPrefab.substr(0, kShortPrefix.size()) == kShortPrefix)
            {
                iconPrefab = iconPrefab.substr(kShortPrefix.size());
            }
            // Prefer the item's actual rig mesh over the icon-derived prefab. The icon is shared across rig variants,
            // so the rule chain is the only place the real cd_phw / cd_pom mesh appears; for an ordinary item the rule
            // mesh equals the icon prefab and this is a no-op.
            // The variant entry list is the authoritative source of every mesh the item uses across bodies; resolve it
            // once and reuse it for both the primary-prefab recovery below and the per-body-variant linkage in PASS 3.
            std::vector<std::string> variantMeshes = resolve_variant_meshes(desc, stringArr, stringCount);

            std::string bodyMesh = resolve_rule_body_mesh(desc, stringArr, stringCount, strip_rig_prefix(iconPrefab));
            std::string resolvedPrefab;
            if (!bodyMesh.empty())
            {
                resolvedPrefab = std::move(bodyMesh);
            }
            else if (isPrefabIcon)
            {
                resolvedPrefab = std::string(iconPrefab); // the `_Prefab_` icon already names the real mesh
            }
            else
            {
                // The icon is not a mesh, so the stripped iconPrefab is a bogus item name and the item would be dropped
                // as UI-only even though a mesh exists. Recover it from the variant entry list -- held / non-body items
                // (a lantern) keep their mesh in an untoken default entry there even when the rule chain is empty. Fall
                // back to the icon name only when the item has no variant mesh.
                resolvedPrefab = variantMeshes.empty() ? std::string(iconPrefab) : variantMeshes.front();
            }
            // Prefer the LIVE partprefab wrapper name. For helm variants the engine instantiates `<mesh>_dd`; the bare
            // `<mesh>` has no live wrapper, so the icon/rule-derived name never matches the swap/carrier path. Rewrite
            // to `_dd` only when the bare form is NOT itself a live wrapper but its `_dd` twin IS -- so ordinary items
            // (whose bare name is the live wrapper) are untouched. See the carrier_defaults.hpp Oongka-helm note.
            if (liveWrapperSet.count(resolvedPrefab) == 0 && liveWrapperSet.count(resolvedPrefab + "_dd") != 0)
                resolvedPrefab += "_dd";
            for (auto &vm : variantMeshes)
            {
                if (liveWrapperSet.count(vm) == 0 && liveWrapperSet.count(vm + "_dd") != 0)
                    vm += "_dd";
            }
            ItemEntry e;
            e.runtimeIdx = id;
            e.internalName = std::move(internalName);
            e.iconSlot = slot;
            e.fullIcon = full;
            e.iconPrefab = std::move(resolvedPrefab);
            e.base = extract_base_prefix(e.iconPrefab);
            e.variantMeshes = std::move(variantMeshes);
            items.push_back(std::move(e));
        }
        logger.info("[itemprefab] item-pass: {} entries collected, {} skipped", items.size(), skipped);

        // PASS 1e -- targeted phantom recovery. Find items whose iconPrefab + base aren't in the pool yet, scan memory
        // once for those exact strings via a first-4-byte hash table. SKIP items whose FullIconString lacks the
        // `_Prefab_` infix (those are UI-only icons -- the targeted search would otherwise resolve quest/abyssgear/etc.
        // strings against unrelated quest-registry entries that happen to share the post-strip name and aren't actual
        // mesh prefabs).
        {
            std::vector<std::string> phantomTargets;
            for (const auto &e : items)
            {
                if (e.iconPrefab.empty())
                    continue;
                if (poolSet.find(e.iconPrefab) != poolSet.end())
                    continue;
                const std::string fullLower = to_lower(e.fullIcon);
                if (is_ui_only_icon(fullLower))
                    continue;
                phantomTargets.push_back(e.iconPrefab);
            }
            std::sort(phantomTargets.begin(), phantomTargets.end());
            phantomTargets.erase(std::unique(phantomTargets.begin(), phantomTargets.end()), phantomTargets.end());
            const auto tT0 = GetTickCount64();
            const auto recovered = recover_phantoms_targeted(poolSet, phantomTargets);
            const auto tT1 = GetTickCount64();
            logger.info("[itemprefab] targeted recovery: {} phantom strings searched, "
                        "{} found in memory ({} ms)",
                        phantomTargets.size(), recovered, tT1 - tT0);
        }

        // A pooled name sourced from a cached or loader-registry string can carry a stray control character (e.g. an
        // embedded CR/LF in an asset name). The byte-scan and stringinfo passes already reject such bytes, but the
        // cached passes do not, so clean every entry through one chokepoint here. Re-inserting into a set both strips
        // the control chars and collapses the dirty variant into its clean twin, so the writer cannot emit a split row
        // or a duplicate key for the same prefab.
        std::set<std::string> cleanPool;
        for (const auto &name : poolSet)
            cleanPool.insert(tsv_sanitize(name));
        std::vector<std::string> pool(cleanPool.begin(), cleanPool.end());

        // Drop dead bare data-name prefabs superseded by a live `_dd` wrapper (helm variants). The bare
        // `..._index01` is a string/data entry with no live partprefab wrapper -- the swap path can never match it, and
        // its items are now linked to the `_dd` row (PASS 2) -- so emitting it as a standalone prefab row is
        // misleading. Guard is narrow: only names whose `_dd` twin is a live wrapper AND that are not themselves a live
        // wrapper are removed.
        {
            const auto before = pool.size();
            pool.erase(std::remove_if(pool.begin(), pool.end(),
                                      [&](const std::string &n)
                                      {
                                          return liveWrapperSet.count(n) == 0 &&
                                                 liveWrapperSet.count(n + "_dd") != 0;
                                      }),
                       pool.end());
            if (pool.size() != before)
                logger.info("[itemprefab] dropped {} dead bare prefab name(s) superseded by `_dd` live wrappers",
                            before - pool.size());
        }

        // PASS 3 -- index for prefab-centric emission.
        // exact_map : pool-prefab -> item index whose iconPrefab equals it.
        // base_map  : base -> all item indices sharing that base.
        std::unordered_map<std::string, size_t> exact_map;
        exact_map.reserve(items.size());
        std::unordered_map<std::string, std::vector<size_t>> base_map;
        base_map.reserve(items.size());
        // Claim every item's primary (icon-derived) prefab FIRST, so a primary link can never be stolen by another
        // item's body variant in the second pass below.
        for (size_t i = 0; i < items.size(); ++i)
        {
            exact_map.emplace(items[i].iconPrefab, i);
            base_map[items[i].base].push_back(i);
        }
        // Then link each item's body-mesh variants (human male / orc / female / NPC) from the variant entry list so
        // their prefab rows resolve to this item instead of orphaning -- the shared, rig-stripped icon can only name
        // one. emplace keeps the first claimant, so a primary link is never overwritten and a mesh shared across items
        // is attributed to (and grouped under) its first owner.
        uint32_t variantLinks = 0;
        for (size_t i = 0; i < items.size(); ++i)
        {
            for (const auto &vm : items[i].variantMeshes)
            {
                if (vm.empty() || vm == items[i].iconPrefab)
                    continue;
                if (exact_map.emplace(vm, i).second)
                {
                    base_map[extract_base_prefix(vm)].push_back(i);
                    ++variantLinks;
                }
            }
        }
        logger.info("[itemprefab] body-mesh variants linked from the variant entry list: {}", variantLinks);

        // PASS 4 -- emit one row per pool prefab. Also emit a row for each item whose iconPrefab is NOT in the pool (so
        // the dump never silently drops an item). Track them via `phantomItems`.
        const auto rtDir = runtime_dir_utf8();
        if (rtDir.empty())
        {
            logger.warning("[itemprefab] runtime dir unavailable");
            return;
        }
        const std::string path = rtDir + "CrimsonDesertLiveTransmog_itemprefabs.tsv";
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            logger.warning("[itemprefab] failed to open output file: {}", path);
            return;
        }

        out << "Prefab\tBase\tExactItemId\tExactItemName\tExactIconSlot\t"
               "SiblingItemIds\tSiblingItemNames\tFullIconString\tOrphan\n";

        std::set<size_t> itemsCovered; // items mentioned in any emitted row

        auto emit_row = [&](std::string_view prefab, std::string_view rowBase, const ItemEntry *exact,
                            const std::vector<size_t> &siblings, bool orphan)
        {
            out << prefab << '\t' << rowBase << '\t';
            if (exact)
            {
                out << exact->runtimeIdx << '\t' << exact->internalName << '\t' << exact->iconSlot;
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
                out << items[siblings[i]].runtimeIdx;
            }
            out << '\t';
            for (size_t i = 0; i < siblings.size(); ++i)
            {
                if (i > 0)
                    out << ',';
                out << items[siblings[i]].internalName;
            }
            out << '\t';
            if (exact)
                out << exact->fullIcon;
            out << '\t' << (orphan ? "yes" : "no") << '\n';
        };

        uint32_t rowsEmitted = 0;
        uint32_t rowsExact = 0;
        uint32_t rowsSiblingOnly = 0;
        uint32_t rowsOrphan = 0;
        uint32_t rowsJunkSkipped = 0;
        for (const auto &p : pool)
        {
            const std::string rowBase = extract_base_prefix(p);
            const ItemEntry *exact = nullptr;
            if (auto it = exact_map.find(p); it != exact_map.end())
            {
                exact = &items[it->second];
                itemsCovered.insert(it->second);
            }
            std::vector<size_t> siblings;
            if (auto it = base_map.find(rowBase); it != base_map.end())
            {
                for (size_t idx : it->second)
                {
                    if (exact && &items[idx] == exact)
                        continue;
                    siblings.push_back(idx);
                    itemsCovered.insert(idx);
                }
            }
            const bool orphan = !exact && siblings.empty();
            // Skip a junk exe-static string fragment (cd_<rig>_<digits><letters>) that resolves to no item -- it is not
            // a real mesh, just noise the cd_-prefix pool walk captured. Guarded on `orphan`, so a linked prefab is
            // never dropped even if one somehow matched the shape.
            if (orphan && is_junk_rig_fragment(p))
            {
                ++rowsJunkSkipped;
                continue;
            }
            if (exact)
                ++rowsExact;
            else if (!siblings.empty())
                ++rowsSiblingOnly;
            else
                ++rowsOrphan;
            emit_row(p, rowBase, exact, siblings, orphan);
            ++rowsEmitted;
        }

        // Phantom items -- their iconPrefab never appeared in the pool (engine's stringinfo references a prefab that
        // the asset-bundle walk missed). Emit ONE row per unique phantom iconPrefab with all sharing items grouped
        // (first as exact, rest as siblings). This dump is PREFAB-based: an item only appears when it resolves to a
        // real mesh/world-object prefab. Mesh-less UI icons (quest/ skill/stat/bespoke `ItemIcon_<name>` with no
        // asset-prefix mesh) are skipped. gimmick/collection/craft/puzzle ARE real prefabs and are kept (see
        // is_ui_only_icon).
        uint32_t phantomRows = 0;
        uint32_t uiSkipped = 0;
        std::set<std::string> phantomIconsEmitted;
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (itemsCovered.count(i))
                continue;
            const std::string fullLower = to_lower(items[i].fullIcon);
            if (is_ui_only_icon(fullLower))
            {
                // Not a real mesh prefab (UI-only icon) -- skip; this dump is prefab-based.
                ++uiSkipped;
                continue;
            }
            if (!phantomIconsEmitted.insert(items[i].iconPrefab).second)
                continue;
            // Collect every other item sharing this iconPrefab via base_map (extract_base_prefix returns the iconPrefab
            // itself when there's no `_NNNN` token, so base lookup is the right grouping key).
            std::vector<size_t> sharing;
            if (auto it = base_map.find(items[i].base); it != base_map.end())
            {
                for (size_t idx : it->second)
                {
                    if (idx == i)
                        continue;
                    if (items[idx].iconPrefab != items[i].iconPrefab)
                        continue;
                    sharing.push_back(idx);
                    itemsCovered.insert(idx);
                }
            }
            emit_row(items[i].iconPrefab, items[i].base, &items[i], sharing, false);
            ++phantomRows;
            ++rowsEmitted;
        }

        logger.info("[itemprefab] dumped {} rows: {} exact, {} sibling-only, "
                    "{} orphan, {} phantom-item, {} UI-only skipped, {} junk-fragment skipped -> "
                    "CrimsonDesertLiveTransmog_itemprefabs.tsv",
                    rowsEmitted, rowsExact, rowsSiblingOnly, rowsOrphan, phantomRows, uiSkipped, rowsJunkSkipped);
    }
} // namespace Transmog
