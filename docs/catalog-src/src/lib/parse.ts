import type { CatalogRow, RowKind } from './types';

const LF = 10;
const CR = 13;

function* splitLines(text: string): Generator<string> {
  let start = 0;
  for (let i = 0; i < text.length; i++) {
    if (text.charCodeAt(i) === LF) {
      const end = i > start && text.charCodeAt(i - 1) === CR ? i - 1 : i;
      yield text.substring(start, end);
      start = i + 1;
    }
  }
  if (start < text.length) {
    const end =
      text.charCodeAt(text.length - 1) === CR ? text.length - 1 : text.length;
    yield text.substring(start, end);
  }
}

export interface DisplayInfo {
  display: string;
  // Wearer-body marking from the display_names 3rd column: 'Male', 'Female', or
  // '' when the item is not body-restricted. This is the item's authoritative
  // equip-eligibility from the game data, unlike a rig-token guess.
  body: string;
}

export function parseDisplayNames(text: string): Map<string, DisplayInfo> {
  const result = new Map<string, DisplayInfo>();
  for (const line of splitLines(text)) {
    if (!line) continue;
    // Columns: <internal name> \t <display name> [ \t <wearer body> ].
    const cols = line.split('\t');
    if (cols.length < 2 || !cols[0]) continue;
    result.set(cols[0], { display: cols[1] ?? '', body: cols[2] ?? '' });
  }
  return result;
}

// Rig family of a mesh: the model/rig token right after `cd_` in the prefab
// (e.g. cd_phm_00_ub -> "phm", cd_m0001_00_samuel_ub -> "m0001", cd_bag_lantern
// -> "bag"). Listed verbatim rather than mapped to a body label, since the token
// is the authoritative family and any human label (male/female/orc) is a guess.
export function rigFamily(prefab: string | undefined): string {
  if (!prefab) return '';
  const p = prefab.toLowerCase();
  if (!p.startsWith('cd_')) return '';
  const rest = p.slice(3);
  const end = rest.indexOf('_');
  return end < 0 ? rest : rest.slice(0, end);
}

// Body-rig classifiers for the duo-body facet. Player/npc rig families follow a
// <role><frame><body> token: [pn] (player/npc) + one or two frame letters
// (h/o/g/d/t...) + a trailing 'w' (female body) or 'm' (male body), e.g. phw /
// pom / nhw / ndem. isFemaleRig requires a single frame letter; isMaleRig allows
// one or two. Unique and monster carriers are m<digits> and count as the
// male/base body; an item's female counterpart is then a phw_ wrapper over the
// same carrier (cd_phw_m0001_00_samuel_ub).
function isFemaleRig(rig: string): boolean {
  return /^[pn][a-z]w$/.test(rig);
}

function isMaleRig(rig: string): boolean {
  return /^[pn][a-z]{1,2}m$/.test(rig) || /^m\d+$/.test(rig);
}

export interface ItemMeta {
  slot: string;
  variant: string;
  playerSafe: boolean;
}

export function parseItems(text: string): Map<number, ItemMeta> {
  const result = new Map<number, ItemMeta>();
  let isHeader = true;
  for (const line of splitLines(text)) {
    if (!line) continue;
    if (isHeader) {
      isHeader = false;
      continue;
    }
    const cols = line.split('\t');
    // Only the first four columns (id, slot, variant, playerSafe) are consumed;
    // the items.tsv Name column is intentionally ignored (row.itemName comes
    // from the prefab dump instead).
    if (cols.length < 4) continue;
    const itemId = parseHexOrDec(cols[0]);
    if (itemId < 0) continue;
    result.set(itemId, {
      slot: cols[1] ?? '',
      variant: cols[2] ?? '',
      playerSafe: (cols[3] ?? '').toLowerCase() === 'yes',
    });
  }
  return result;
}

function parseHexOrDec(raw: string | undefined): number {
  if (!raw) return -1;
  const value = raw.trim();
  if (!value) return -1;
  if (value.startsWith('0x') || value.startsWith('0X')) {
    const parsed = parseInt(value.substring(2), 16);
    return Number.isFinite(parsed) ? parsed : -1;
  }
  const parsed = parseInt(value, 10);
  return Number.isFinite(parsed) ? parsed : -1;
}

function parseCsvIds(raw: string | undefined): number[] | undefined {
  if (!raw) return undefined;
  const ids: number[] = [];
  for (const part of raw.split(',')) {
    const parsed = parseHexOrDec(part);
    if (parsed >= 0) ids.push(parsed);
  }
  return ids.length ? ids : undefined;
}

function parseCsvStrings(raw: string | undefined): string[] | undefined {
  if (!raw) return undefined;
  const trimmed = raw
    .split(',')
    .map((part) => part.trim())
    .filter(Boolean);
  return trimmed.length ? trimmed : undefined;
}

function buildSearchBlob(parts: (string | number | undefined)[]): string {
  const segments: string[] = [];
  for (const part of parts) {
    if (part === undefined || part === null) continue;
    const text = typeof part === 'number' ? part.toString() : part;
    if (text) segments.push(text.toLowerCase());
  }
  return segments.join('|');
}

export interface JoinResult {
  rows: CatalogRow[];
  nameOnlyCount: number;
  orphanCount: number;
  siblingsOnlyCount: number;
  emptyCount: number;
  fullMatchCount: number;
}

export function joinAll(
  displayText: string,
  prefabText: string,
  itemsText?: string,
): JoinResult {
  const displayMap = parseDisplayNames(displayText);
  const itemsMap = itemsText
    ? parseItems(itemsText)
    : new Map<number, ItemMeta>();

  const rows: CatalogRow[] = [];
  const seenItemNames = new Set<string>();
  // itemId -> number of prefab rows whose EXACT item is this id. Drives the
  // multi-prefab facet: an item that is the exact item of more than one mesh
  // prefab (e.g. a character armor with separate male and female body meshes for
  // the same item) has count > 1. Sibling references are deliberately excluded:
  // a sibling link is a speculative alternate mapping, not a confirmed mesh
  // attached to the item, so counting it would flag distinct set pieces (e.g.
  // Iguana_Rider_PlateArmor_Armor_I/II/III, which merely list each other as
  // siblings) as multi-prefab.
  const itemPrefabCount = new Map<number, number>();
  // itemId -> which body rigs its exact prefabs use, for the duo-body facet. An
  // item that is the exact item of both a female-body mesh and a male/base-body
  // mesh renders on both bodies (e.g. Samuel armor: cd_m0001_ male + cd_phw_
  // female).
  const itemBodyRigs = new Map<number, { female: boolean; male: boolean }>();
  let isHeader = true;
  let nextId = 0;
  let orphanCount = 0;
  let siblingsOnlyCount = 0;
  let emptyCount = 0;
  let fullMatchCount = 0;

  for (const line of splitLines(prefabText)) {
    if (!line) continue;
    if (isHeader) {
      isHeader = false;
      continue;
    }
    const cols = line.split('\t');
    if (cols.length < 1) continue;

    const prefab = cols[0] || undefined;
    const base = cols[1] || undefined;
    const itemId = parseHexOrDec(cols[2]);
    const itemName = cols[3] || undefined;
    const iconSlot = parseHexOrDec(cols[4]);
    const siblingItemIds = parseCsvIds(cols[5]);
    const siblingItemNames = parseCsvStrings(cols[6]);
    const fullIconString = cols[7] || undefined;
    const isOrphan = (cols[8] ?? '').toLowerCase() === 'yes';

    const info = itemName ? displayMap.get(itemName) : undefined;
    const displayName = info?.display;
    // Body eligibility from the display_names mark: 'Male' / 'Female' when the
    // item is restricted to one body, undefined otherwise (unmarked or no entry).
    const bodyType = info?.body || undefined;
    if (itemName) seenItemNames.add(itemName);

    let siblingDisplayNames: (string | undefined)[] | undefined;
    if (siblingItemNames) {
      const resolved: (string | undefined)[] = [];
      let anyResolved = false;
      for (const siblingName of siblingItemNames) {
        seenItemNames.add(siblingName);
        const sibDisplay = displayMap.get(siblingName)?.display;
        resolved.push(sibDisplay);
        if (sibDisplay) anyResolved = true;
      }
      if (anyResolved) siblingDisplayNames = resolved;
    }

    const meta = itemId >= 0 ? itemsMap.get(itemId) : undefined;
    const rig = rigFamily(prefab);

    // Tally exact prefab references per item for the multi-prefab facet, and
    // record which body rigs the item's exact prefabs use for the duo-body
    // facet. Sibling references are deliberately not counted (see above).
    if (itemId >= 0) {
      itemPrefabCount.set(itemId, (itemPrefabCount.get(itemId) ?? 0) + 1);
      const flags = itemBodyRigs.get(itemId) ?? { female: false, male: false };
      if (isFemaleRig(rig)) flags.female = true;
      else if (isMaleRig(rig)) flags.male = true;
      itemBodyRigs.set(itemId, flags);
    }

    let kind: RowKind;
    if (isOrphan) {
      kind = 'prefab-orphan';
      orphanCount++;
    } else if (itemName) {
      kind = 'prefab+name';
      fullMatchCount++;
    } else if (siblingItemNames?.length) {
      kind = 'prefab+siblings';
      siblingsOnlyCount++;
    } else {
      kind = 'prefab-empty';
      emptyCount++;
    }

    rows.push({
      id: nextId++,
      kind,
      prefab,
      base,
      itemId: itemId >= 0 ? itemId : undefined,
      itemName,
      displayName,
      iconSlot: iconSlot >= 0 ? iconSlot : undefined,
      fullIconString,
      siblingItemIds,
      siblingItemNames,
      siblingDisplayNames,
      slot: meta?.slot,
      variant: meta?.variant,
      playerSafe: meta?.playerSafe,
      rigFamily: rig,
      bodyType,
      searchBlob: buildSearchBlob([
        prefab,
        base,
        itemName,
        displayName,
        itemId >= 0 ? itemId : undefined,
        itemId >= 0 ? `0x${itemId.toString(16)}` : undefined,
        fullIconString,
        siblingItemNames?.join(' '),
        siblingDisplayNames?.filter(Boolean).join(' '),
        meta?.slot,
        rig,
      ]),
    });
  }

  // Second pass: attach each prefab row's multi-prefab count and duo-body flag
  // from its exact item now that all exact references have been tallied. Rows
  // without an exact item (orphans, sibling-only, name-only) are neither, so
  // their fields stay undefined.
  for (const row of rows) {
    if (row.itemId === undefined) continue;
    row.prefabCount = itemPrefabCount.get(row.itemId) ?? 0;
    const flags = itemBodyRigs.get(row.itemId);
    row.duoBody = !!(flags && flags.female && flags.male);
  }

  let nameOnlyCount = 0;
  for (const [itemName, info] of displayMap) {
    if (seenItemNames.has(itemName)) continue;
    nameOnlyCount++;
    rows.push({
      id: nextId++,
      kind: 'name-only',
      itemName,
      displayName: info.display,
      bodyType: info.body || undefined,
      searchBlob: buildSearchBlob([itemName, info.display]),
    });
  }

  return {
    rows,
    nameOnlyCount,
    orphanCount,
    siblingsOnlyCount,
    emptyCount,
    fullMatchCount,
  };
}
