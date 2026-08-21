<script setup lang="ts">
/*
 * One hit.
 *
 * The snippet arrives as HTML because the marks around the matched words are
 * produced by the scorer, which knows where they are. It is escaped at that
 * point (search.ts, `escapeHtml`) and nothing but <mark> survives.
 */
import type { SearchResult } from '@/lib/search'
import AppIcon from '@/components/ui/AppIcon.vue'

defineProps<{
  result: SearchResult
  selected: boolean
}>()
</script>

<template>
  <li class="hit" :class="{ 'hit--selected': selected }">
    <RouterLink class="hit__link" :to="result.record.to">
      <AppIcon
        class="hit__icon"
        :name="result.record.kind === 'feature' ? 'chevron' : 'hash'"
        :size="14"
      />
      <span class="hit__body">
        <span class="hit__head">
          <span class="hit__title">{{ result.record.title }}</span>
          <span class="hit__context">{{ result.record.context }}</span>
        </span>
        <!-- eslint-disable-next-line vue/no-v-html -->
        <span v-if="result.snippet" class="hit__snippet" v-html="result.snippet" />
      </span>
    </RouterLink>
  </li>
</template>

<style scoped lang="scss">
.hit {
  &__link {
    display: flex;
    gap: var(--space-3);
    padding: var(--space-3);
    border-radius: var(--radius-md);
    color: inherit;
    text-decoration: none;
    outline: none;
  }

  &--selected .hit__link {
    background: var(--c-accent-soft);
    box-shadow: inset 0 0 0 1px var(--c-accent);
  }

  &__icon {
    margin-top: 0.25em;
    color: var(--c-text-faint);
  }

  &__body {
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: var(--space-1);
  }

  &__head {
    display: flex;
    align-items: baseline;
    gap: var(--space-2);
    flex-wrap: wrap;
  }

  &__title {
    color: var(--c-text);
    font-weight: var(--weight-medium);
  }

  &__context {
    @include eyebrow;
  }

  &__snippet {
    color: var(--c-text-muted);
    font-size: var(--text-base);
    line-height: var(--leading-snug);
    @include line-clamp(2);
  }
}
</style>
