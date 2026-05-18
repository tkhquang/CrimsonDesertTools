export type RowKind =
  | 'prefab+name'
  | 'prefab+siblings'
  | 'prefab-orphan'
  | 'name-only'
  | 'prefab-empty';

export interface CatalogRow {
  id: number;
  kind: RowKind;
  prefab?: string;
  base?: string;
  itemId?: number;
  itemName?: string;
  displayName?: string;
  iconSlot?: number;
  fullIconString?: string;
  siblingItemIds?: number[];
  siblingItemNames?: string[];
  // Parallel to siblingItemNames; undefined at index i means that sibling
  // has no entry in display_names.tsv. Aligning indices lets the details
  // panel pair each sibling id with its name without a second lookup.
  siblingDisplayNames?: (string | undefined)[];
  slot?: string;
  variant?: string;
  playerSafe?: boolean;
  searchBlob: string;
}

export interface FacetState {
  hasPrefab: 'any' | 'yes' | 'no';
  hasDisplayName: 'any' | 'yes' | 'no';
  hasInternalName: 'any' | 'yes' | 'no';
  hasItemId: 'any' | 'yes' | 'no';
  hasSiblings: 'any' | 'yes' | 'no';
  orphanOnly: boolean;
  emptyOnly: boolean;
  slot: string;
}

export const DEFAULT_FACETS: FacetState = {
  hasPrefab: 'any',
  hasDisplayName: 'any',
  hasInternalName: 'any',
  hasItemId: 'any',
  hasSiblings: 'any',
  orphanOnly: false,
  emptyOnly: false,
  slot: '',
};

// Slot tag substrings mirrored from
// CrimsonDesertLiveTransmog/src/prefab_wrapper_swap.cpp (k_slotTagPatterns).
// A row matches a slot if items.tsv 'Slot' equals the name OR any pattern
// is a substring of the prefab. Earring1/2 and Ring1/2 are collapsed to
// single user-facing names since the cpp duplicates them only to provide
// independent equip slots; the prefab substring tag is identical.
export interface SlotDefinition {
  name: string;
  patterns: string[];
}

export const SLOT_DEFINITIONS: readonly SlotDefinition[] = [
  { name: 'Helm', patterns: ['_hel_'] },
  { name: 'Chest', patterns: ['_ub_'] },
  { name: 'Cloak', patterns: ['_cloak_'] },
  { name: 'Gloves', patterns: ['_hand_'] },
  { name: 'Boots', patterns: ['_foot_'] },
  { name: 'Earring', patterns: ['_earring_'] },
  { name: 'Necklace', patterns: ['_necklace_'] },
  { name: 'Ring', patterns: ['_ring_'] },
  { name: 'Lantern', patterns: ['_lantern_'] },
  { name: 'Glasses', patterns: ['_glasses_'] },
  { name: 'Mask', patterns: ['_mask_00_'] },
  { name: 'Backpack', patterns: ['_bag_0'] },
  { name: 'Bracelet', patterns: ['_rinkband_'] },
  { name: 'MainHand', patterns: ['_phm_01_', '_phw_01_'] },
  { name: 'OffHand', patterns: ['_phm_01_', '_phw_01_', '_03_shield_'] },
  {
    name: 'Ranged',
    patterns: [
      '_phm_04_',
      '_phw_04_',
      '_phm_06_',
      '_phw_06_',
      '_phm_13_',
      '_phw_13_',
    ],
  },
  { name: 'SubWeapon', patterns: ['_phm_01_dagger_', '_phw_01_dagger_'] },
  { name: '2H Weapon', patterns: ['_phm_02_', '_phw_02_'] },
] as const;

const SLOT_INDEX = new Map<string, SlotDefinition>(
  SLOT_DEFINITIONS.map((definition) => [
    definition.name.toLowerCase(),
    definition,
  ]),
);

// Pre-filter for slot pattern matching. The cpp pulls candidates from real
// item registries, so it never sees knowledge-image references like
// `cd_knowledgeimage_..._cd_phm_00_hel_00_0363_c`, UI icons, background
// props, or horse parts. This regex restricts substring matching to prefabs
// whose root is a recognized character or monster model:
//   cd_ph[mw]_  player male/female
//   cd_nh[mw]_  NPC humanoid male/female
//   cd_nt[mw]_  NPC type-m/w
//   cd_m<n>_    monster carriers
const REAL_ITEM_PREFIX_RE = /^cd_(ph[mw]|nh[mw]|nt[mw]|m\d+)_/;

export function matchesSlot(
  slotName: string,
  rowSlot: string | undefined,
  rowPrefab: string | undefined,
): boolean {
  const target = slotName.toLowerCase();
  if (rowSlot && rowSlot.toLowerCase() === target) return true;
  if (!rowPrefab) return false;
  const prefabLower = rowPrefab.toLowerCase();
  if (!REAL_ITEM_PREFIX_RE.test(prefabLower)) return false;
  const definition = SLOT_INDEX.get(target);
  if (!definition) return false;
  for (const pattern of definition.patterns) {
    if (prefabLower.includes(pattern)) return true;
  }
  return false;
}

export type QuickPreset =
  | 'all'
  | 'name-and-prefab'
  | 'name-only'
  | 'orphans'
  | 'siblings-only';

export const KIND_LABEL: Record<RowKind, string> = {
  'prefab+name': 'Full match',
  'prefab+siblings': 'Sibling-linked',
  'prefab-orphan': 'Orphan prefab',
  'name-only': 'Name only',
  'prefab-empty': 'Empty row',
};

export const KIND_COLOR: Record<RowKind, string> = {
  'prefab+name': 'border-l-green',
  'prefab+siblings': 'border-l-yellow',
  'prefab-orphan': 'border-l-red',
  'name-only': 'border-l-orange',
  'prefab-empty': 'border-l-text-dim',
};
