<script lang="ts">
  import { catalog } from '../store.svelte';
  import type { QuickPreset } from '../types';

  const presets: { id: QuickPreset; label: string; hint: string }[] = [
    { id: 'all', label: 'All', hint: 'No filters' },
    {
      id: 'name-and-prefab',
      label: 'Name + Prefab',
      hint: 'Items with both a display name and a prefab',
    },
    {
      id: 'name-only',
      label: 'Items without prefab',
      hint: 'Display-name rows missing from the prefab dump',
    },
    {
      id: 'orphans',
      label: 'Orphan prefabs',
      hint: 'Prefabs with no item link at all',
    },
    {
      id: 'siblings-only',
      label: 'Sibling-linked',
      hint: 'Prefabs with siblings but no exact item',
    },
  ];
</script>

<div class="flex flex-wrap items-center gap-1.5">
  <span class="mr-1 text-xs text-text-dim">Quick:</span>
  {#each presets as preset (preset.id)}
    <button
      type="button"
      title={preset.hint}
      onclick={() => catalog.applyPreset(preset.id)}
      class="cursor-pointer rounded border border-border bg-panel px-2.5 py-1 text-xs hover:border-accent hover:text-accent"
    >
      {preset.label}
    </button>
  {/each}
  <button
    type="button"
    onclick={() => catalog.resetFilters()}
    class="ml-2 cursor-pointer rounded border border-border bg-panel px-2.5 py-1 text-xs text-text-dim hover:border-yellow hover:text-yellow"
  >
    Reset
  </button>
</div>
