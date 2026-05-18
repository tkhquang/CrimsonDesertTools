<script lang="ts">
  import type { CatalogRow } from '../types';
  import { KIND_LABEL } from '../types';
  import { catalog } from '../store.svelte';

  type Props = { row: CatalogRow };
  let { row }: Props = $props();

  let copyMsg = $state('');

  async function copy(text: string) {
    try {
      await navigator.clipboard.writeText(text);
      copyMsg = `Copied: ${text.slice(0, 40)}${text.length > 40 ? '…' : ''}`;
      setTimeout(() => (copyMsg = ''), 1500);
    } catch {
      copyMsg = 'Copy failed';
    }
  }

  function close() {
    catalog.selected = null;
  }
</script>

<aside class="flex h-full w-105 shrink-0 flex-col border-l border-separator bg-panel">
  <div class="flex items-center justify-between border-b border-separator px-3 py-2">
    <div class="text-sm font-semibold">{KIND_LABEL[row.kind]}</div>
    <button
      type="button"
      onclick={close}
      aria-label="Close details"
      class="cursor-pointer rounded px-2 py-0.5 text-text-dim hover:bg-input hover:text-text"
    >
      ✕
    </button>
  </div>

  <div class="flex-1 space-y-3 overflow-auto p-3 text-xs">
    {#if row.prefab}
      <div>
        <div class="text-text-dim">Prefab</div>
        <button class="block w-full break-all text-left font-mono text-text hover:text-accent" onclick={() => copy(row.prefab!)}>{row.prefab}</button>
      </div>
    {/if}
    {#if row.base && row.base !== row.prefab}
      <div>
        <div class="text-text-dim">Base</div>
        <button class="block w-full break-all text-left font-mono text-text hover:text-accent" onclick={() => copy(row.base!)}>{row.base}</button>
      </div>
    {/if}
    {#if row.itemName}
      <div>
        <div class="text-text-dim">Internal name</div>
        <button class="block w-full break-all text-left font-mono text-text hover:text-accent" onclick={() => copy(row.itemName!)}>{row.itemName}</button>
      </div>
    {/if}
    {#if row.displayName}
      <div>
        <div class="text-text-dim">Display name</div>
        <div class="text-text">{row.displayName}</div>
      </div>
    {/if}
    {#if row.itemId !== undefined}
      <div>
        <div class="text-text-dim">Item ID</div>
        <div class="font-mono text-text">{row.itemId} <span class="text-text-dim">(0x{row.itemId.toString(16)})</span></div>
      </div>
    {/if}
    {#if row.iconSlot !== undefined}
      <div>
        <div class="text-text-dim">Icon slot</div>
        <div class="font-mono text-text">{row.iconSlot}</div>
      </div>
    {/if}
    {#if row.fullIconString}
      <div>
        <div class="text-text-dim">Full icon string</div>
        <button class="block w-full break-all text-left font-mono text-text hover:text-accent" onclick={() => copy(row.fullIconString!)}>{row.fullIconString}</button>
      </div>
    {/if}
    {#if row.slot}
      <div>
        <div class="text-text-dim">Slot · Variant</div>
        <div class="text-text">{row.slot}{#if row.variant} · {row.variant}{/if}{#if row.playerSafe} · <span class="text-green">player-safe</span>{/if}</div>
      </div>
    {/if}
    {#if row.siblingItemNames && row.siblingItemNames.length}
      <div>
        <div class="text-text-dim">Siblings ({row.siblingItemNames.length})</div>
        <ul class="space-y-1 font-mono">
          {#each row.siblingItemNames as name, i (name + i)}
            <li class="flex items-baseline gap-2">
              {#if row.siblingItemIds?.[i] !== undefined}
                <span class="shrink-0 text-text-dim">{row.siblingItemIds[i]}</span>
              {/if}
              <button class="break-all text-left text-text hover:text-accent" onclick={() => copy(name)}>{name}</button>
              {#if row.siblingDisplayNames?.[i]}
                <span class="text-text-dim">· {row.siblingDisplayNames[i]}</span>
              {/if}
            </li>
          {/each}
        </ul>
      </div>
    {/if}
  </div>

  {#if copyMsg}
    <div class="border-t border-separator px-3 py-1.5 text-xs text-green">{copyMsg}</div>
  {/if}
</aside>
