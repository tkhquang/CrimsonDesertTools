<script lang="ts">
  import type { Snippet } from 'svelte';

  type Props = {
    count: number;
    rowHeight?: number;
    overscan?: number;
    item: Snippet<[number]>;
    empty?: Snippet;
  };

  let {
    count,
    rowHeight = 56,
    overscan = 8,
    item,
    empty,
  }: Props = $props();

  let scrollEl = $state<HTMLDivElement | undefined>(undefined);
  let scrollTop = $state(0);
  let viewportHeight = $state(0);

  const totalSize = $derived(count * rowHeight);

  const startIndex = $derived(
    Math.max(0, Math.floor(scrollTop / rowHeight) - overscan),
  );
  const visibleCount = $derived(
    Math.ceil(viewportHeight / rowHeight) + overscan * 2,
  );
  const endIndex = $derived(Math.min(count, startIndex + visibleCount));

  const visibleIndices = $derived.by(() => {
    const indices: number[] = [];
    for (let i = startIndex; i < endIndex; i++) indices.push(i);
    return indices;
  });

  function onScroll() {
    if (scrollEl) scrollTop = scrollEl.scrollTop;
  }

  $effect(() => {
    if (!scrollEl) return;
    viewportHeight = scrollEl.clientHeight;
    const observer = new ResizeObserver(() => {
      if (scrollEl) viewportHeight = scrollEl.clientHeight;
    });
    observer.observe(scrollEl);
    return () => observer.disconnect();
  });

  // When count shrinks (filter applied), clamp scrollTop so the viewport
  // doesn't sit past the end of the (now shorter) content.
  $effect(() => {
    if (!scrollEl) return;
    const maxScroll = Math.max(0, totalSize - viewportHeight);
    if (scrollEl.scrollTop > maxScroll) {
      scrollEl.scrollTop = maxScroll;
      scrollTop = maxScroll;
    }
  });
</script>

<div
  bind:this={scrollEl}
  onscroll={onScroll}
  class="relative h-full w-full overflow-y-auto overflow-x-hidden scrollbar-gutter-stable"
>
  {#if count === 0}
    <div class="flex h-full items-center justify-center text-text-dim">
      {#if empty}{@render empty()}{:else}No results{/if}
    </div>
  {:else}
    <div class="relative w-full" style="height: {totalSize}px;">
      {#each visibleIndices as index (index)}
        <div
          class="absolute left-0 top-0 w-full"
          style="height: {rowHeight}px; transform: translateY({index *
            rowHeight}px);"
        >
          {@render item(index)}
        </div>
      {/each}
    </div>
  {/if}
</div>
