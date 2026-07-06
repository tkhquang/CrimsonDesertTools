<script lang="ts">
  import { catalog, supportsBodyFacets } from '../store.svelte';
  import { SLOT_DEFINITIONS, BODY_TYPES } from '../types';

  type TriState = 'any' | 'yes' | 'no';

  // The Body, Multi-prefab, and Duo-body facets rely on data only the v1.13.00+
  // dumps carry, so their controls are shown only on supported versions (the
  // store also clears their values on an unsupported dump).
  const showBodyFacets = $derived(supportsBodyFacets(catalog.version));

  const triFacets: {
    key: keyof typeof catalog.facets;
    label: string;
    title: string;
  }[] = [
    {
      key: 'hasPrefab',
      label: 'Prefab',
      title: 'Row has a prefab name from itemprefabs.tsv',
    },
    {
      key: 'hasDisplayName',
      label: 'Display name',
      title:
        'Row has a human-readable name resolved via display_names.tsv (e.g. "Awrain Plate Helm")',
    },
    {
      key: 'hasInternalName',
      label: 'Internal name',
      title:
        'Row has an engine-level item identifier (e.g. "Aant_PlateArmor_Helm")',
    },
    {
      key: 'hasItemId',
      label: 'Item ID',
      title: 'Row has a numeric item ID',
    },
    {
      key: 'hasSiblings',
      label: 'Siblings',
      title:
        'Prefab is sibling-linked to one or more items via SiblingItemNames',
    },
  ] as const;

  const multiPrefabTitle =
    'Item is the exact item of more than one mesh prefab (one item with ' +
    'several distinct meshes, e.g. a character armor with separate male and ' +
    'female body meshes). Sibling links do not count.';

  function cycle(current: TriState): TriState {
    return current === 'any' ? 'yes' : current === 'yes' ? 'no' : 'any';
  }

  function colorFor(state: TriState): string {
    if (state === 'yes') return 'border-green text-green';
    if (state === 'no') return 'border-red text-red';
    return 'border-border text-text-dim';
  }

  function symbolFor(state: TriState): string {
    return state === 'yes' ? '✓' : state === 'no' ? '✕' : '·';
  }
</script>

<div class="flex flex-wrap items-center gap-1.5">
  <span class="mr-1 text-xs text-text-dim">Facets:</span>
  {#each triFacets as facet (facet.key)}
    {@const current = catalog.facets[facet.key] as TriState}
    <button
      type="button"
      onclick={() => {
        (catalog.facets[facet.key] as TriState) = cycle(current);
      }}
      class="cursor-pointer rounded border bg-panel px-2 py-1 text-xs hover:bg-input {colorFor(current)}"
      title="{facet.title}. Click to cycle: any, yes, no."
    >
      <span class="mr-1 font-mono">{symbolFor(current)}</span>{facet.label}
    </button>
  {/each}

  <label class="ml-2 inline-flex cursor-pointer items-center gap-1 rounded border border-border bg-panel px-2 py-1 text-xs hover:bg-input">
    <input
      type="checkbox"
      bind:checked={catalog.facets.orphanOnly}
      class="accent-accent"
    />
    Orphan only
  </label>

  <label class="inline-flex cursor-pointer items-center gap-1 rounded border border-border bg-panel px-2 py-1 text-xs hover:bg-input">
    <input
      type="checkbox"
      bind:checked={catalog.facets.emptyOnly}
      class="accent-accent"
    />
    Empty only
  </label>

  {#if showBodyFacets}
    <label
      class="inline-flex cursor-pointer items-center gap-1 rounded border border-border bg-panel px-2 py-1 text-xs hover:bg-input"
    >
      <input
        type="checkbox"
        bind:checked={catalog.facets.duoBodyOnly}
        class="accent-accent"
      />
      <span
        title={'Duo-body items only: the item is the exact item of both a\n' +
          'female-body mesh (cd_phw_ / cd_pow_ / ...) and a male or base-body\n' +
          'mesh (cd_phm_ / cd_pom_ / cd_m<n>_ carrier), e.g. Samuel armor.'}
      >
        Duo-body
      </span>
    </label>
  {/if}

  <label class="ml-2 inline-flex items-center gap-1 text-xs text-text-dim">
    <span>Slot</span>
    <span
      class="cursor-help text-yellow"
      title={'Heuristic. Matches the items.tsv Slot field OR a slot-tag substring in the prefab\n' +
        '(mirrored from prefab_wrapper_swap.cpp k_slotTagPatterns).\n\n' +
        'Pattern matching is pre-filtered to prefabs starting with a real model prefix\n' +
        '(cd_phm_/cd_phw_/cd_nh[mw]_/cd_nt[mw]_/cd_m<n>_) so knowledge-image references,\n' +
        'UI icons, background props, and horse parts are excluded.\n\n' +
        'Remaining caveats:\n' +
        '• Patterns overlap (MainHand and OffHand both match _phm_01_)\n' +
        '• Real prefabs without the standard tag are missed (false negatives)\n' +
        '• Real prefabs with the tag in an unexpected position can false-positive\n\n' +
        'For exact control, use the regex search instead (.* button).'}
      aria-label="Slot filter accuracy note"
    >
      (!)
    </span>
    <select
      bind:value={catalog.facets.slot}
      class="cursor-pointer rounded border border-border bg-input px-1.5 py-0.5 text-xs text-text outline-none focus:border-accent"
    >
      <option value="">all</option>
      {#each SLOT_DEFINITIONS as definition (definition.name)}
        <option value={definition.name}>{definition.name}</option>
      {/each}
    </select>
  </label>

  {#if showBodyFacets}
    {@const multiPrefab = catalog.facets.multiPrefab}
    <button
      type="button"
      onclick={() => {
        catalog.facets.multiPrefab = cycle(multiPrefab);
      }}
      class="cursor-pointer rounded border bg-panel px-2 py-1 text-xs hover:bg-input {colorFor(multiPrefab)}"
      title="{multiPrefabTitle} Click to cycle: any, yes, no."
    >
      <span class="mr-1 font-mono">{symbolFor(multiPrefab)}</span>Multi-prefab
    </button>

    <label class="inline-flex items-center gap-1 text-xs text-text-dim">
      <span>Body</span>
      <span
        class="cursor-help text-yellow"
        title={'Wearer body from the display_names data (authoritative, not a\n' +
          'rig-token guess): Male / Female for items restricted to that body.\n' +
          'Items with no body restriction are unmarked and only match Any\n' +
          '(the default, which does not filter).'}
        aria-label="Body filter note"
      >
        (!)
      </span>
      <select
        bind:value={catalog.facets.bodyType}
        class="cursor-pointer rounded border border-border bg-input px-1.5 py-0.5 text-xs text-text outline-none focus:border-accent"
      >
        <option value="">any</option>
        {#each BODY_TYPES as body (body)}
          <option value={body}>{body}</option>
        {/each}
      </select>
    </label>
  {/if}
</div>
