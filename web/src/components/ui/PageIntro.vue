<script setup lang="ts">
/*
 * The block at the top of a page: an optional eyebrow, the title, and one
 * paragraph. Every page uses this, so the vertical rhythm between a title and
 * the first thing under it is decided once.
 */
defineProps<{
  eyebrow?: string
  title: string
}>()
</script>

<template>
  <header class="intro">
    <p v-if="eyebrow" class="intro__eyebrow">{{ eyebrow }}</p>
    <h1 class="intro__title">{{ title }}</h1>
    <div v-if="$slots.default" class="intro__lead">
      <slot />
    </div>
  </header>
</template>

<style scoped lang="scss">
.intro {
  &__eyebrow {
    @include eyebrow;
    margin-bottom: var(--space-3);
  }

  &__title {
    font-size: var(--text-3xl);
    max-width: 24ch;
  }

  &__lead {
    margin-top: var(--space-4);
    max-width: var(--measure-prose);
    color: var(--c-text-muted);
    font-size: var(--text-lg);
    line-height: var(--leading-relaxed);

    :deep(p + p) {
      margin-top: var(--space-3);
    }
  }
}
</style>
