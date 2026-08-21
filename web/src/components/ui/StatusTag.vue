<script setup lang="ts">
/*
 * Whether a feature is finished.
 *
 * Only shown when it is not: a tag on everything would be noise, and "working"
 * is what a feature on the list is expected to be. `partial` is worth the row
 * it takes, because the alternative is a DJ finding out on a deck.
 */
import type { FeatureStatus } from '@/content/features'

defineProps<{ status: FeatureStatus }>()
</script>

<template>
  <span v-if="status === 'partial'" class="status" title="Built, with a known limitation">
    partial
  </span>
</template>

<style scoped lang="scss">
.status {
  @include eyebrow;
  padding: 0.1rem var(--space-2);
  border: var(--border-hair) solid var(--c-border-strong);
  border-radius: var(--radius-pill);
  color: var(--c-text-muted);
  white-space: nowrap;
}
</style>
