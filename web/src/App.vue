<script setup lang="ts">
import SiteHeader from '@/components/layout/SiteHeader.vue'
import SiteFooter from '@/components/layout/SiteFooter.vue'
import SearchDialog from '@/components/search/SearchDialog.vue'
import { ref } from 'vue'

const searchOpen = ref(false)
</script>

<template>
  <a class="skip-link" href="#main">Skip to content</a>

  <SiteHeader @open-search="searchOpen = true" />

  <main id="main" class="app-main" tabindex="-1">
    <RouterView />
  </main>

  <SiteFooter />

  <SearchDialog v-model:open="searchOpen" />
</template>

<style scoped lang="scss">
.app-main {
  /* The docs reader manages its own height; everything else just flows. */
  min-height: 60vh;
  outline: none;
}
</style>
