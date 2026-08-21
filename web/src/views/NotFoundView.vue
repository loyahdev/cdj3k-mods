<script setup lang="ts">
import { useRoute } from 'vue-router'

const route = useRoute()
</script>

<template>
  <div class="missing">
    <p class="missing__code">404</p>
    <h1 class="missing__title">No page at this address</h1>
    <p class="missing__path">
      <code>{{ route.fullPath }}</code>
    </p>
    <p class="missing__links">
      <RouterLink to="/">Overview</RouterLink>
      <RouterLink to="/features">Features</RouterLink>
      <RouterLink to="/docs">Documentation</RouterLink>
    </p>
  </div>
</template>

<style scoped lang="scss">
.missing {
  @include container();
  padding-block: var(--space-10);
  text-align: center;

  &__code {
    @include eyebrow;
  }

  &__title {
    margin-top: var(--space-3);
    font-size: var(--text-2xl);
  }

  &__path {
    margin-top: var(--space-3);
    color: var(--c-text-faint);
    word-break: break-all;
  }

  &__links {
    display: flex;
    justify-content: center;
    gap: var(--space-5);
    margin-top: var(--space-6);
  }
}
</style>
