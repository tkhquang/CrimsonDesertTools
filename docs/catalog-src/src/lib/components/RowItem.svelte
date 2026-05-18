<script lang="ts">
  import type { CatalogRow } from '../types';
  import { KIND_LABEL, KIND_COLOR } from '../types';
  import { catalog } from '../store.svelte';

  type Props = { row: CatalogRow };
  let { row }: Props = $props();

  const isSelected = $derived(catalog.selected?.id === row.id);

  const primary = $derived(
    row.displayName ?? row.itemName ?? row.prefab ?? '(unknown)',
  );

  const secondaryTokens = $derived.by(() => {
    const tokens: { text: string; mono?: boolean }[] = [];
    if (row.itemName && row.itemName !== primary) {
      tokens.push({ text: row.itemName, mono: true });
    }
    if (
      row.prefab &&
      row.prefab !== primary &&
      row.prefab !== row.itemName
    ) {
      tokens.push({ text: row.prefab, mono: true });
    }
    if (!tokens.length) {
      if (row.kind === 'prefab-orphan') tokens.push({ text: 'orphan prefab' });
      else if (row.kind === 'prefab-empty') tokens.push({ text: 'empty row' });
      else if (row.kind === 'name-only') tokens.push({ text: 'no prefab' });
    }
    return tokens;
  });

  const siblingCount = $derived(row.siblingItemNames?.length ?? 0);
</script>

<button
  type="button"
  onclick={() => (catalog.selected = row)}
  class="flex h-full w-full items-stretch border-b border-separator border-l-4 pl-3 pr-5 py-1.5 text-left hover:bg-panel {KIND_COLOR[
    row.kind
  ]} {isSelected ? 'bg-panel' : ''}"
>
  <div class="min-w-0 flex-1">
    <div class="truncate font-medium text-text">{primary}</div>
    <div class="truncate text-xs text-text-dim">
      {#each secondaryTokens as token, i (token.text + i)}
        {#if i > 0}<span class="px-1.5 text-text-disabled">•</span>{/if}<span
          class={token.mono ? 'font-mono' : ''}>{token.text}</span
        >
      {/each}
      {#if siblingCount > 0}
        <span class="px-1.5 text-text-disabled">•</span>
        <span class="text-yellow">+{siblingCount} sibling{siblingCount === 1 ? '' : 's'}</span>
      {/if}
    </div>
  </div>
  <div
    class="ml-3 flex shrink-0 flex-col items-end justify-center text-xs"
  >
    {#if row.itemId !== undefined}
      <span class="text-text-dim">ID {row.itemId}</span>
    {/if}
    <span class="text-text-disabled" title={KIND_LABEL[row.kind]}>
      {#if row.slot}{row.slot}{:else}{KIND_LABEL[row.kind]}{/if}
    </span>
  </div>
</button>
