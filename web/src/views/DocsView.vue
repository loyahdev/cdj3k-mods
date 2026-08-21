<script setup lang="ts">
/*
 * The reader's shell: the document list on the left, the current document in
 * the outlet. Mounted once for the whole /docs subtree, so switching document
 * does not rebuild the navigation or lose its scroll position.
 */
import { ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import DocsNav from '@/components/docs/DocsNav.vue'
import AppIcon from '@/components/ui/AppIcon.vue'

const route = useRoute()
const navOpen = ref(false)

watch(() => route.fullPath, () => (navOpen.value = false))
</script>

<template>
  <div class="docs">
    <button
      type="button"
      class="docs__toggle"
      :aria-expanded="navOpen"
      aria-controls="docs-nav"
      @click="navOpen = !navOpen"
    >
      <AppIcon :name="navOpen ? 'close' : 'book'" :size="16" />
      {{ navOpen ? 'Close' : 'All documents' }}
    </button>

    <aside id="docs-nav" class="docs__aside" :class="{ 'docs__aside--open': navOpen }">
      <div class="docs__aside-inner">
        <DocsNav />
      </div>
    </aside>

    <div class="docs__outlet">
      <RouterView />
    </div>
  </div>
</template>

<style scoped lang="scss">
.docs {
  @include container(var(--width-wide));
  display: grid;
  gap: var(--space-6);
  padding-block: var(--space-5) var(--space-8);
  grid-template-columns: 1fr;

  @include from('lg') {
    grid-template-columns: var(--width-docs-nav) minmax(0, 1fr);
    gap: var(--space-8);
    padding-block: var(--space-7) var(--space-9);
  }

  &__toggle {
    display: inline-flex;
    align-items: center;
    gap: var(--space-2);
    align-self: start;
    padding: var(--space-2) var(--space-3);
    border: var(--border-hair) solid var(--c-border);
    border-radius: var(--radius-md);
    background: var(--c-surface);
    color: var(--c-text-muted);
    font-size: var(--text-base);
    @include focus-ring(1px);

    @include from('lg') {
      display: none;
    }
  }

  &__aside {
    @include below('lg') {
      display: none;

      &--open {
        display: block;
      }
    }

    @include from('lg') {
      /* Sticky, not scroll-driven: the browser holds it, nothing recomputes a
       * position as the page moves. */
      position: sticky;
      top: calc(var(--header-height) + var(--space-5));
      align-self: start;
      max-height: calc(100dvh - var(--header-height) - var(--space-8));
      overflow-y: auto;
      @include thin-scrollbar;
    }
  }

  &__aside-inner {
    @include below('lg') {
      padding: var(--space-4);
      background: var(--c-surface);
      border: var(--border-hair) solid var(--c-border);
      border-radius: var(--radius-lg);
    }
  }

  &__outlet {
    min-width: 0;
  }
}
</style>
