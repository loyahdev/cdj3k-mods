/// <reference types="vite/client" />

declare module '*.vue' {
  import type { DefineComponent } from 'vue'
  const component: DefineComponent<Record<string, never>, Record<string, never>, unknown>
  export default component
}

declare module 'virtual:docs' {
  import type { DocMeta } from '@/types/docs'
  export const docs: DocMeta[]
  /** One per document, keyed by slug. Each resolves that document's HTML. */
  export const loaders: Record<string, () => Promise<{ html: string }>>
}

declare module 'virtual:docs-search' {
  import type { DocSearchEntry } from '@/types/docs'
  export const entries: DocSearchEntry[]
}
