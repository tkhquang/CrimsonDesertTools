import type { CatalogRow, FacetState, QuickPreset } from './types';
import { DEFAULT_FACETS, matchesSlot } from './types';
import { joinAll, type JoinResult } from './parse';

export const CATALOG_VERSION = 'v1.07.00';
const DISPLAY_NAMES_URL = `./CrimsonDesertLiveTransmog_display_names_${CATALOG_VERSION}.tsv`;
const ITEM_PREFABS_URL = `./CrimsonDesertLiveTransmog_itemprefabs_${CATALOG_VERSION}.tsv`;
const ITEMS_URL = `./CrimsonDesertLiveTransmog_items_${CATALOG_VERSION}.tsv`;
const SEARCH_DEBOUNCE_MS = 120;
export const MAX_PATTERN_LENGTH = 200;

export type LoadStatus = 'idle' | 'loading' | 'ready' | 'error';

interface Stats {
  total: number;
  fullMatch: number;
  siblingsOnly: number;
  orphan: number;
  nameOnly: number;
  empty: number;
}

class CatalogStore {
  rows = $state<CatalogRow[]>([]);
  status = $state<LoadStatus>('idle');
  error = $state<string>('');
  stats = $state<Stats>({
    total: 0,
    fullMatch: 0,
    siblingsOnly: 0,
    orphan: 0,
    nameOnly: 0,
    empty: 0,
  });
  query = $state<string>('');
  debouncedQuery = $state<string>('');
  facets = $state<FacetState>({ ...DEFAULT_FACETS });
  selected = $state<CatalogRow | null>(null);
  useRegex = $state<boolean>(false);
  compiledRegex = $state<RegExp | null>(null);
  regexError = $state<string>('');

  #debounceTimer: ReturnType<typeof setTimeout> | undefined;

  setQuery(next: string) {
    this.query = next;
    if (this.#debounceTimer !== undefined) clearTimeout(this.#debounceTimer);
    this.#debounceTimer = setTimeout(() => {
      this.debouncedQuery = next.trim();
      this.#recompileRegex();
    }, SEARCH_DEBOUNCE_MS);
  }

  setUseRegex(enabled: boolean) {
    this.useRegex = enabled;
    this.#recompileRegex();
  }

  #recompileRegex() {
    if (!this.useRegex || !this.debouncedQuery) {
      this.compiledRegex = null;
      this.regexError = '';
      return;
    }
    if (this.debouncedQuery.length > MAX_PATTERN_LENGTH) {
      this.compiledRegex = null;
      this.regexError = `Pattern too long (max ${MAX_PATTERN_LENGTH} chars)`;
      return;
    }
    try {
      this.compiledRegex = new RegExp(this.debouncedQuery, 'i');
      this.regexError = '';
    } catch (cause) {
      this.compiledRegex = null;
      this.regexError =
        cause instanceof Error ? cause.message : String(cause);
    }
  }

  resetFilters() {
    this.facets = { ...DEFAULT_FACETS };
    this.query = '';
    this.debouncedQuery = '';
    this.useRegex = false;
    this.compiledRegex = null;
    this.regexError = '';
  }

  // Read shareable filter/search state from URL params. Bypasses the search
  // debounce so the filter takes effect on the first paint after navigation.
  applyUrlState(params: URLSearchParams) {
    if (this.#debounceTimer !== undefined) {
      clearTimeout(this.#debounceTimer);
      this.#debounceTimer = undefined;
    }
    const query = params.get('q') ?? '';
    this.query = query;
    this.debouncedQuery = query.trim();
    this.facets = {
      hasPrefab: triStateFromParam(params.get('prefab')),
      hasDisplayName: triStateFromParam(params.get('displayName')),
      hasInternalName: triStateFromParam(params.get('internalName')),
      hasItemId: triStateFromParam(params.get('itemId')),
      hasSiblings: triStateFromParam(params.get('siblings')),
      orphanOnly: params.get('orphan') === '1',
      emptyOnly: params.get('empty') === '1',
      slot: params.get('slot') ?? '',
    };
    this.setUseRegex(params.get('regex') === '1');
  }

  // Serialize current filter/search state into URL params. Default values are
  // omitted so a shared link stays short and only encodes the user's actual
  // selections.
  toUrlParams(): URLSearchParams {
    const params = new URLSearchParams();
    if (this.debouncedQuery) params.set('q', this.debouncedQuery);
    if (this.useRegex) params.set('regex', '1');
    const facets = this.facets;
    if (facets.hasPrefab !== 'any') params.set('prefab', facets.hasPrefab);
    if (facets.hasDisplayName !== 'any')
      params.set('displayName', facets.hasDisplayName);
    if (facets.hasInternalName !== 'any')
      params.set('internalName', facets.hasInternalName);
    if (facets.hasItemId !== 'any') params.set('itemId', facets.hasItemId);
    if (facets.hasSiblings !== 'any') params.set('siblings', facets.hasSiblings);
    if (facets.orphanOnly) params.set('orphan', '1');
    if (facets.emptyOnly) params.set('empty', '1');
    if (facets.slot) params.set('slot', facets.slot);
    return params;
  }

  applyPreset(preset: QuickPreset) {
    this.facets = { ...DEFAULT_FACETS };
    switch (preset) {
      case 'all':
        break;
      case 'name-and-prefab':
        this.facets.hasPrefab = 'yes';
        this.facets.hasDisplayName = 'yes';
        break;
      case 'name-only':
        this.facets.hasPrefab = 'no';
        this.facets.hasDisplayName = 'yes';
        break;
      case 'orphans':
        this.facets.orphanOnly = true;
        break;
      case 'siblings-only':
        this.facets.hasInternalName = 'no';
        this.facets.hasSiblings = 'yes';
        break;
    }
  }

  async load(source?: { display: string; prefabs: string; items?: string }) {
    this.status = 'loading';
    this.error = '';
    try {
      const [displayText, prefabText, itemsText] = source
        ? [source.display, source.prefabs, source.items]
        : await Promise.all([
            fetchText(DISPLAY_NAMES_URL),
            fetchText(ITEM_PREFABS_URL),
            fetchText(ITEMS_URL).catch(() => undefined),
          ]);
      const result = joinAll(displayText, prefabText, itemsText);
      this.#applyResult(result);
      this.status = 'ready';
    } catch (cause) {
      this.error = cause instanceof Error ? cause.message : String(cause);
      this.status = 'error';
    }
  }

  async loadFromFiles(files: {
    display: File;
    prefabs: File;
    items?: File;
  }): Promise<void> {
    const [display, prefabs, items] = await Promise.all([
      files.display.text(),
      files.prefabs.text(),
      files.items ? files.items.text() : Promise.resolve(undefined),
    ]);
    await this.load({ display, prefabs, items });
  }

  #applyResult(result: JoinResult) {
    this.rows = result.rows;
    this.stats = {
      total: result.rows.length,
      fullMatch: result.fullMatchCount,
      siblingsOnly: result.siblingsOnlyCount,
      orphan: result.orphanCount,
      nameOnly: result.nameOnlyCount,
      empty: result.emptyCount,
    };
  }
}

function triStateFromParam(value: string | null): 'any' | 'yes' | 'no' {
  return value === 'yes' || value === 'no' ? value : 'any';
}

async function fetchText(url: string): Promise<string> {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
  return response.text();
}

function matchesFacets(row: CatalogRow, facets: FacetState): boolean {
  if (facets.orphanOnly && row.kind !== 'prefab-orphan') return false;
  if (facets.emptyOnly && row.kind !== 'prefab-empty') return false;

  if (facets.hasPrefab !== 'any') {
    const hasPrefab = !!row.prefab;
    if ((facets.hasPrefab === 'yes') !== hasPrefab) return false;
  }
  if (facets.hasDisplayName !== 'any') {
    const hasDisplayName = !!row.displayName;
    if ((facets.hasDisplayName === 'yes') !== hasDisplayName) return false;
  }
  if (facets.hasInternalName !== 'any') {
    const hasInternalName = !!row.itemName;
    if ((facets.hasInternalName === 'yes') !== hasInternalName) return false;
  }
  if (facets.hasItemId !== 'any') {
    const hasItemId = row.itemId !== undefined;
    if ((facets.hasItemId === 'yes') !== hasItemId) return false;
  }
  if (facets.hasSiblings !== 'any') {
    const hasSiblings = !!row.siblingItemNames?.length;
    if ((facets.hasSiblings === 'yes') !== hasSiblings) return false;
  }
  if (facets.slot && !matchesSlot(facets.slot, row.slot, row.prefab)) {
    return false;
  }
  return true;
}

export const catalog = new CatalogStore();

export function hasActiveFilter(): boolean {
  if (catalog.debouncedQuery) return true;
  const facets = catalog.facets;
  if (facets.orphanOnly || facets.emptyOnly || facets.slot) return true;
  return (
    facets.hasPrefab !== 'any' ||
    facets.hasDisplayName !== 'any' ||
    facets.hasInternalName !== 'any' ||
    facets.hasItemId !== 'any' ||
    facets.hasSiblings !== 'any'
  );
}

// Two-way sync between catalog filter state and the URL query string.
// Must be called from a Svelte component context so the $effect handles
// can be torn down on unmount.
export function installUrlSync(): void {
  catalog.applyUrlState(new URL(window.location.href).searchParams);

  $effect(() => {
    const queryString = catalog.toUrlParams().toString();
    const url = `${window.location.pathname}${
      queryString ? `?${queryString}` : ''
    }${window.location.hash}`;
    window.history.replaceState(window.history.state, '', url);
  });

  $effect(() => {
    const onPopState = () => {
      catalog.applyUrlState(new URL(window.location.href).searchParams);
    };
    window.addEventListener('popstate', onPopState);
    return () => window.removeEventListener('popstate', onPopState);
  });
}

export function filteredRows(): CatalogRow[] {
  const query = catalog.debouncedQuery;
  const useRegex = catalog.useRegex;
  const regex = catalog.compiledRegex;
  const facets = catalog.facets;
  const source = catalog.rows;

  // If regex compilation failed on a non-empty pattern, return an empty
  // result so the user sees the error (already rendered in SearchBar) rather
  // than the unfiltered list, which would suggest the filter is off.
  if (useRegex && query && !regex) return [];

  const queryLower = useRegex ? '' : query.toLowerCase();
  const matches: CatalogRow[] = [];
  for (let i = 0; i < source.length; i++) {
    const row = source[i];
    if (!matchesFacets(row, facets)) continue;
    if (query) {
      if (useRegex) {
        if (!regex!.test(row.searchBlob)) continue;
      } else if (!row.searchBlob.includes(queryLower)) {
        continue;
      }
    }
    matches.push(row);
  }
  return matches;
}
