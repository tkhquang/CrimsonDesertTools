<script lang="ts">
  import { catalog, MAX_PATTERN_LENGTH } from '../store.svelte';

  function onInput(event: Event) {
    catalog.setQuery((event.currentTarget as HTMLInputElement).value);
  }

  function clear() {
    catalog.setQuery('');
  }

  function toggleRegex() {
    catalog.setUseRegex(!catalog.useRegex);
  }
</script>

<div class="w-full">
  <div class="flex items-stretch gap-1.5">
    <div class="relative flex-1">
      <input
        type="text"
        placeholder={catalog.useRegex
          ? 'Regex pattern (case-insensitive) e.g. ^cd_phm_.*_helm'
          : 'Search prefab, name, internal name, item id…'}
        value={catalog.query}
        oninput={onInput}
        maxlength={MAX_PATTERN_LENGTH}
        spellcheck="false"
        autocomplete="off"
        autocapitalize="off"
        class="w-full rounded border bg-input px-3 py-2 text-text outline-none placeholder:text-text-dim focus:border-accent {catalog.useRegex &&
        catalog.regexError
          ? 'border-red'
          : 'border-border'} {catalog.useRegex ? 'font-mono' : ''}"
      />
      {#if catalog.query}
        <button
          type="button"
          onclick={clear}
          aria-label="Clear search"
          class="absolute right-2 top-1/2 -translate-y-1/2 cursor-pointer rounded px-2 py-0.5 text-xs text-text-dim hover:bg-border hover:text-text"
        >
          ✕
        </button>
      {/if}
    </div>
    <button
      type="button"
      onclick={toggleRegex}
      title={catalog.useRegex
        ? 'Regex mode ON. Click to switch to plain text.'
        : 'Plain text. Click to enable regex.'}
      aria-pressed={catalog.useRegex}
      class="cursor-pointer rounded border px-3 font-mono text-xs {catalog.useRegex
        ? 'border-accent bg-input text-accent'
        : 'border-border bg-panel text-text-dim hover:border-accent hover:text-accent'}"
    >
      .*
    </button>
  </div>
  {#if catalog.useRegex && catalog.regexError}
    <div class="mt-1 flex items-start gap-1 text-xs text-red">
      <span aria-hidden="true">⚠</span>
      <span class="font-mono">{catalog.regexError}</span>
    </div>
  {/if}
</div>
