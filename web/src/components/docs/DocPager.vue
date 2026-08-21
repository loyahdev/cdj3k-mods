<script setup lang="ts">
/*
 * Previous / next, in the reading order from content/docs.ts.
 */
import type { Doc } from '@/content/docs'
import AppIcon from '@/components/ui/AppIcon.vue'

defineProps<{
  prev?: Doc
  next?: Doc
}>()
</script>

<template>
  <nav v-if="prev || next" class="pager" aria-label="Documents">
    <RouterLink v-if="prev" class="pager__link pager__link--prev" :to="`/docs/${prev.slug}`">
      <span class="pager__label"><AppIcon name="arrowLeft" :size="14" /> Previous</span>
      <span class="pager__title">{{ prev.title }}</span>
    </RouterLink>
    <span v-else />

    <RouterLink v-if="next" class="pager__link pager__link--next" :to="`/docs/${next.slug}`">
      <span class="pager__label">Next <AppIcon name="arrowRight" :size="14" /></span>
      <span class="pager__title">{{ next.title }}</span>
    </RouterLink>
  </nav>
</template>

<style scoped lang="scss">
.pager {
  display: grid;
  grid-template-columns: 1fr;
  gap: var(--space-3);
  margin-top: var(--space-8);
  padding-top: var(--space-5);
  border-top: var(--border-hair) solid var(--c-border);

  @include from('sm') {
    grid-template-columns: 1fr 1fr;
  }

  &__link {
    @include card;
    display: flex;
    flex-direction: column;
    gap: var(--space-1);
    padding: var(--space-3) var(--space-4);
    text-decoration: none;
    transition: border-color var(--duration-fast) var(--ease-out);
    @include focus-ring(2px);

    &:hover {
      border-color: var(--c-accent);
    }

    &--next {
      text-align: end;
      align-items: flex-end;
    }
  }

  &__label {
    @include eyebrow;
    display: inline-flex;
    align-items: center;
    gap: var(--space-1);
  }

  &__title {
    color: var(--c-text);
    font-size: var(--text-base);
    font-weight: var(--weight-medium);
  }
}
</style>
