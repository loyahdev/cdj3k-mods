<script setup lang="ts">
import { site } from '@/content/site'
</script>

<template>
  <footer class="footer">
    <div class="footer__inner">
      <p class="footer__line">
        A community project. {{ site.device }}, firmware {{ site.firmware }}.
        Not affiliated with, endorsed by, or connected to AlphaTheta Corporation
        or Pioneer DJ. Trademarks belong to their owners. Installing voids your
        warranty; no warranty is given and no liability accepted.
        <RouterLink to="/docs/legal">Legal</RouterLink>
      </p>
      <ul class="footer__links">
        <li><a :href="site.repo" target="_blank" rel="noopener noreferrer">Source</a></li>
        <li><a :href="site.releases" target="_blank" rel="noopener noreferrer">Download</a></li>
        <li><a :href="site.stemd" target="_blank" rel="noopener noreferrer">stemd</a></li>
        <li class="footer__licence">{{ site.license }}</li>
      </ul>
    </div>
  </footer>
</template>

<style scoped lang="scss">
.footer {
  margin-top: var(--space-10);
  border-top: var(--border-hair) solid var(--c-border);

  &__inner {
    @include container(var(--width-wide));
    display: flex;
    flex-direction: column;
    gap: var(--space-4);
    padding-block: var(--space-6);

    @include from('lg') {
      flex-direction: row;
      align-items: baseline;
      justify-content: space-between;
      gap: var(--space-6);
    }
  }

  &__line {
    max-width: 62ch;
    color: var(--c-text-faint);
    font-size: var(--text-xs);
    line-height: var(--leading-normal);
  }

  &__links {
    display: flex;
    flex-wrap: wrap;
    gap: var(--space-4);
    list-style: none;
    padding: 0;
    margin: 0;
    font-size: var(--text-xs);

    a {
      color: var(--c-text-muted);
      text-decoration: none;
      @include focus-ring(2px);

      &:hover {
        color: var(--c-accent);
        text-decoration: underline;
      }
    }
  }

  &__licence {
    color: var(--c-text-faint);
    font-family: var(--font-mono);
  }
}
</style>
