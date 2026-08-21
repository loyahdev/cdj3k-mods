<script setup lang="ts">
import type { FeatureGroup } from '@/content/features'
import FeatureCard from './FeatureCard.vue'

defineProps<{ group: FeatureGroup }>()
</script>

<template>
  <section :id="group.id" class="group" :aria-labelledby="`${group.id}-title`">
    <header class="group__head">
      <h2 :id="`${group.id}-title`" class="group__title">{{ group.title }}</h2>
      <p class="group__note">{{ group.note }}</p>
    </header>

    <div class="group__grid">
      <FeatureCard v-for="feature in group.features" :key="feature.id" :feature="feature" />
    </div>
  </section>
</template>

<style scoped lang="scss">
.group {
  scroll-margin-top: calc(var(--header-height) + var(--space-5));

  &__head {
    margin-bottom: var(--space-5);
    padding-bottom: var(--space-4);
    border-bottom: var(--border-hair) solid var(--c-border);
  }

  &__title {
    font-size: var(--text-2xl);
  }

  &__note {
    max-width: var(--measure-prose);
    margin-top: var(--space-2);
    color: var(--c-text-muted);
    line-height: var(--leading-relaxed);
  }

  &__grid {
    display: grid;
    gap: var(--space-4);
    grid-template-columns: 1fr;

    @include from('md') {
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }

    @include from('xl') {
      grid-template-columns: repeat(3, minmax(0, 1fr));
    }
  }
}
</style>
