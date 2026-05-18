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

export function parseDisplayNames(text: string): Map<string, string> {
  const result = new Map<string, string>();
  for (const line of splitLines(text)) {
    if (!line) continue;
    const tabIndex = line.indexOf('\t');
    if (tabIndex < 0) continue;
    const itemName = line.substring(0, tabIndex);
    const displayName = line.substring(tabIndex + 1);
    result.set(itemName, displayName);
  }
  return result;
}

export interface ItemMeta {
  slot: string;
  variant: string;
  playerSafe: boolean;
  name: string;
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
    if (cols.length < 5) continue;
    const itemId = parseHexOrDec(cols[0]);
    if (itemId < 0) continue;
    result.set(itemId, {
      slot: cols[1] ?? '',
      variant: cols[2] ?? '',
      playerSafe: (cols[3] ?? '').toLowerCase() === 'yes',
      name: cols[4] ?? '',
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

    const displayName = itemName ? displayMap.get(itemName) : undefined;
    if (itemName) seenItemNames.add(itemName);

    let siblingDisplayNames: (string | undefined)[] | undefined;
    if (siblingItemNames) {
      const resolved: (string | undefined)[] = [];
      let anyResolved = false;
      for (const siblingName of siblingItemNames) {
        seenItemNames.add(siblingName);
        const sibDisplay = displayMap.get(siblingName);
        resolved.push(sibDisplay);
        if (sibDisplay) anyResolved = true;
      }
      if (anyResolved) siblingDisplayNames = resolved;
    }

    const meta = itemId >= 0 ? itemsMap.get(itemId) : undefined;

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
      ]),
    });
  }

  let nameOnlyCount = 0;
  for (const [itemName, displayName] of displayMap) {
    if (seenItemNames.has(itemName)) continue;
    nameOnlyCount++;
    rows.push({
      id: nextId++,
      kind: 'name-only',
      itemName,
      displayName,
      searchBlob: buildSearchBlob([itemName, displayName]),
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
