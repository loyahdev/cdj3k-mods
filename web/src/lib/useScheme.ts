/*
 * Colour scheme.
 *
 * Three states, not two: `auto` follows the OS, and the other two are an
 * explicit choice that outranks it. Written to the root element as a data
 * attribute, which is what tokens/_color.scss keys on, and remembered in
 * localStorage so the choice survives a reload.
 *
 * The first application happens in index.html, before Vue mounts, so the page
 * never paints in the wrong scheme and then corrects itself.
 */
import { ref, watch, readonly } from 'vue'

export type Scheme = 'auto' | 'light' | 'dark'

const STORAGE_KEY = 'cdj3k-scheme'

function read(): Scheme {
  try {
    const saved = localStorage.getItem(STORAGE_KEY)
    if (saved === 'light' || saved === 'dark') return saved
  } catch {
    /* private mode */
  }
  return 'auto'
}

const scheme = ref<Scheme>(read())

function apply(value: Scheme) {
  const root = document.documentElement
  if (value === 'auto') delete root.dataset.scheme
  else root.dataset.scheme = value

  try {
    if (value === 'auto') localStorage.removeItem(STORAGE_KEY)
    else localStorage.setItem(STORAGE_KEY, value)
  } catch {
    /* private mode: the choice lasts for this page only */
  }
}

watch(scheme, apply)

/** Whether the page is currently dark, resolving `auto` against the OS. */
function resolved(): 'light' | 'dark' {
  if (scheme.value !== 'auto') return scheme.value
  return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
}

export function useScheme() {
  return {
    scheme: readonly(scheme),
    /** Flip to the opposite of what is on screen, whichever way `auto` resolved. */
    toggle() {
      scheme.value = resolved() === 'dark' ? 'light' : 'dark'
    },
    set(value: Scheme) {
      scheme.value = value
    },
    resolved,
  }
}
