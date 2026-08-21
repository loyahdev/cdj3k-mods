<script setup lang="ts">
import AppIcon from '@/components/ui/AppIcon.vue'
import KeyHint from '@/components/ui/KeyHint.vue'
</script>

<template>
  <button type="button" class="trigger" aria-label="Search the documentation">
    <AppIcon name="search" :size="16" />
    <span class="trigger__label">Search</span>
    <KeyHint class="trigger__keys" :keys="['mod', 'K']" />
  </button>
</template>

<style scoped lang="scss">
.trigger {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  height: 2rem;
  padding-inline: var(--space-2);
  border: var(--border-hair) solid var(--c-border);
  border-radius: var(--radius-md);
  background: var(--c-surface-raised);
  color: var(--c-text-faint);
  font-size: var(--text-base);
  transition:
    border-color var(--duration-fast) var(--ease-out),
    color var(--duration-fast) var(--ease-out);

  @include focus-ring(1px);

  &:hover {
    border-color: var(--c-border-strong);
    color: var(--c-text-muted);
  }

  /* Below the breakpoint it collapses to the icon: the label and the shortcut
   * are both meaningless on a device with no keyboard. */
  &__label,
  &__keys {
    display: none;
  }

  @include from('md') {
    padding-inline: var(--space-3);
    min-width: 13rem;

    .trigger__label {
      display: block;
      margin-inline-end: auto;
    }
    .trigger__keys {
      display: inline-flex;
    }
  }
}
</style>
