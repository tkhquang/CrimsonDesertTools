<script lang="ts">
  import { onMount } from 'svelte';
  import {
    catalog,
    filteredRows,
    hasActiveFilter,
    installUrlSync,
    CATALOG_VERSIONS,
    UNCORRECTED_VERSIONS,
    DEFAULT_VERSION,
  } from './lib/store.svelte';
  import SearchBar from './lib/components/SearchBar.svelte';
  import QuickPresets from './lib/components/QuickPresets.svelte';
  import FacetChips from './lib/components/FacetChips.svelte';
  import VirtualList from './lib/components/VirtualList.svelte';
  import RowItem from './lib/components/RowItem.svelte';
  import DetailsPanel from './lib/components/DetailsPanel.svelte';

  const ROW_HEIGHT = 56;

  const visibleRows = $derived(filteredRows());
  const isFiltered = $derived(hasActiveFilter());
  const matchPercent = $derived(
    catalog.stats.total > 0
      ? (visibleRows.length / catalog.stats.total) * 100
      : 0,
  );

  const isUncorrectedVersion = $derived(
    UNCORRECTED_VERSIONS.includes(catalog.version),
  );

  let displayInput: HTMLInputElement | undefined = $state();
  let prefabInput: HTMLInputElement | undefined = $state();
  let itemsInput: HTMLInputElement | undefined = $state();

  installUrlSync();

  onMount(() => {
    catalog.load();
  });

  async function loadFromPickers() {
    const display = displayInput?.files?.[0];
    const prefabs = prefabInput?.files?.[0];
    const items = itemsInput?.files?.[0];
    if (!display || !prefabs) {
      catalog.error = 'Need both display_names.tsv and itemprefabs.tsv';
      catalog.status = 'error';
      return;
    }
    await catalog.loadFromFiles({ display, prefabs, items });
  }
</script>

<div class="flex h-screen flex-col">
  <header class="flex items-center gap-3 border-b border-separator px-3 py-2">
    <h1 class="text-sm font-semibold">Live Transmog · Catalog</h1>
    <label class="text-xs text-text-dim" title="Game version / TSV dump to load">
      <select
        value={catalog.version}
        onchange={(e) => catalog.setVersion(e.currentTarget.value as typeof CATALOG_VERSIONS[number])}
        class="cursor-pointer rounded border border-border bg-input px-1 py-0.5 text-xs text-text outline-none focus:border-accent"
      >
        {#each CATALOG_VERSIONS as version}
          <option value={version}>{version}</option>
        {/each}
      </select>
    </label>

    <div class="ml-auto flex items-center gap-2 text-xs">
      {#if catalog.status === 'ready'}
        <span class="text-green">
          {catalog.stats.total.toLocaleString()} rows loaded
        </span>
      {:else if catalog.status === 'loading'}
        <span class="text-yellow">Loading…</span>
      {:else if catalog.status === 'error'}
        <span class="text-red" title={catalog.error}>Load error</span>
      {/if}
      <button
        type="button"
        onclick={() => catalog.load()}
        class="cursor-pointer rounded border border-border bg-panel px-2 py-0.5 hover:border-accent hover:text-accent"
        title={`Refetch the bundled ${catalog.version} TSVs`}
      >
        Reload
      </button>
    </div>
  </header>

  {#if isUncorrectedVersion}
    <div
      class="border-b border-separator bg-input px-3 py-2 text-xs text-orange"
      role="alert"
    >
      ⚠ <span class="font-semibold">{catalog.version}</span> is an older dump
      and its data may be incomplete. Dumps before
      <span class="font-semibold">{DEFAULT_VERSION}</span> were generated
      before two catalog fixes: items whose names use special characters
      (e.g. Roman numerals) can be missing, and some character-specific gear
      meshes (male / female / orc rig variants) can be mislinked. Select
      <span class="font-semibold">{DEFAULT_VERSION}</span> for the corrected
      data.
    </div>
  {/if}

  {#if catalog.status === 'error'}
    <div class="border-b border-separator bg-input px-3 py-2 text-xs text-red">
      {catalog.error}
      <details class="mt-1 text-text-dim">
        <summary class="cursor-pointer">Load a different TSV set</summary>
        <div class="mt-2 flex flex-wrap items-center gap-2">
          <label class="flex items-center gap-1">
            display_names
            <input
              bind:this={displayInput}
              type="file"
              accept=".tsv,text/tab-separated-values"
              class="text-text"
            />
          </label>
          <label class="flex items-center gap-1">
            itemprefabs
            <input
              bind:this={prefabInput}
              type="file"
              accept=".tsv,text/tab-separated-values"
              class="text-text"
            />
          </label>
          <label class="flex items-center gap-1">
            items (optional)
            <input
              bind:this={itemsInput}
              type="file"
              accept=".tsv,text/tab-separated-values"
              class="text-text"
            />
          </label>
          <button
            type="button"
            onclick={loadFromPickers}
            class="cursor-pointer rounded border border-accent bg-panel px-2 py-0.5 text-accent hover:bg-input"
          >
            Load
          </button>
        </div>
      </details>
    </div>
  {/if}

  <div class="flex flex-col gap-2 border-b border-separator px-3 py-2">
    <div class="flex items-center gap-2">
      <div class="flex-1">
        <SearchBar />
      </div>
      <div class="shrink-0 text-xs">
        {#if isFiltered}
          <span class="font-semibold text-accent">
            {visibleRows.length.toLocaleString()}
          </span>
          <span class="text-text-dim">
            match{visibleRows.length === 1 ? '' : 'es'} of {catalog.stats.total.toLocaleString()}
            ({matchPercent.toFixed(matchPercent < 1 ? 2 : 1)}%)
          </span>
        {:else}
          <span class="text-text-dim">
            {catalog.stats.total.toLocaleString()} rows
          </span>
        {/if}
      </div>
    </div>
    <QuickPresets />
    <FacetChips />
  </div>

  <div class="flex min-h-0 flex-1">
    <div class="min-w-0 flex-1">
      {#if catalog.status === 'ready'}
        <VirtualList count={visibleRows.length} rowHeight={ROW_HEIGHT}>
          {#snippet item(index)}
            <RowItem row={visibleRows[index]} />
          {/snippet}
          {#snippet empty()}
            <div class="text-center">
              <div class="text-sm text-text">No matches</div>
              <button
                type="button"
                onclick={() => catalog.resetFilters()}
                class="mt-2 cursor-pointer text-xs text-accent hover:text-accent-hover"
              >
                Reset filters
              </button>
            </div>
          {/snippet}
        </VirtualList>
      {:else if catalog.status === 'loading'}
        <div class="flex h-full items-center justify-center text-text-dim">
          Loading catalog…
        </div>
      {/if}
    </div>

    {#if catalog.selected}
      <DetailsPanel row={catalog.selected} />
    {/if}
  </div>

  <footer
    class="flex items-center justify-between border-t border-separator px-3 py-1.5 text-xs text-text-dim"
  >
    <div>
      <span class="text-green">●</span>
      {catalog.stats.fullMatch.toLocaleString()} full
      ·
      <span class="text-yellow">●</span>
      {catalog.stats.siblingsOnly.toLocaleString()} sibling-only
      ·
      <span class="text-red">●</span>
      {catalog.stats.orphan.toLocaleString()} orphan
      ·
      <span class="text-orange">●</span>
      {catalog.stats.nameOnly.toLocaleString()} name-only
      ·
      <span>{catalog.stats.empty.toLocaleString()} empty</span>
    </div>
    <a
      href="./index.html"
      class="text-text-dim hover:text-accent"
    >
      ← Preset builder
    </a>
  </footer>
</div>
