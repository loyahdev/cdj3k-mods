<script setup lang="ts">
/*
 * One document: the article, its contents column, and the pager.
 *
 * The body is fetched as its own chunk, so the title, lead and contents list
 * are on screen from the manifest before the prose arrives.
 *
 * The hash has to be resolved here rather than in the router's scrollBehavior.
 * A link into another document navigates before that document's chunk has
 * loaded, so the target heading does not exist yet when the router looks for
 * it; this waits until the article is actually in the DOM.
 */
import { computed, nextTick, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import { getDoc, docNeighbours, loadDocHtml } from '@/content/docs'
import DocArticle from '@/components/docs/DocArticle.vue'
import DocToc from '@/components/docs/DocToc.vue'
import DocPager from '@/components/docs/DocPager.vue'
import ImageLightbox from '@/components/docs/ImageLightbox.vue'

const props = defineProps<{ slug: string }>()

const route = useRoute()
const articleEl = ref<HTMLElement | null>(null)
const lightbox = ref<InstanceType<typeof ImageLightbox> | null>(null)
const html = ref('')

const doc = computed(() => getDoc(props.slug))
const neighbours = computed(() => docNeighbours(props.slug))

/* The document title is set by the router -- see its afterEach. */

watch(
  () => props.slug,
  async (slug) => {
    html.value = ''
    if (!getDoc(slug)) return

    const loaded = await loadDocHtml(slug)
    // A fast reader can have moved on while this was in flight.
    if (props.slug !== slug) return
    html.value = loaded
    await settleVideos()
    await goToHash()
  },
  { immediate: true },
)

/*
 * Autoplay is an attribute the build has to write, so a reader who prefers
 * reduced motion is honoured here instead: the clips are paused and rewound
 * rather than removed, and their controls still work.
 */
async function settleVideos() {
  await nextTick()
  if (!window.matchMedia('(prefers-reduced-motion: reduce)').matches) return
  for (const v of document.querySelectorAll<HTMLVideoElement>('.md-video video')) {
    v.autoplay = false
    v.pause()
    v.currentTime = 0
  }
}

/* Delegated, because the article is raw HTML from the build: there is no
 * component per image to put a handler on. Videos are excluded -- a click there
 * belongs to their own controls. */
function openImage(event: MouseEvent) {
  const target = event.target as HTMLElement | null
  if (!target || target.tagName !== 'IMG') return
  const img = target as HTMLImageElement
  lightbox.value?.open(img.currentSrc || img.src, img.alt)
}

async function goToHash() {
  await nextTick()
  const hash = route.hash
  if (!hash) return
  const el = document.querySelector(hash)
  if (el) el.scrollIntoView({ block: 'start' })
}

/* A contents click inside the document already on screen. */
watch(() => route.hash, goToHash)
</script>

<template>
  <div v-if="doc" class="doc-page">
    <div class="doc-page__main">
      <header class="doc-page__head">
        <h1 class="doc-page__title">{{ doc.title }}</h1>
        <p v-if="doc.lead" class="doc-page__lead">{{ doc.lead }}</p>
      </header>

      <div ref="articleEl" @click="openImage">
        <DocArticle v-if="html" :html="html" />
        <p v-else class="doc-page__loading">Loading…</p>
      </div>

      <DocPager :prev="neighbours.prev" :next="neighbours.next" />
    </div>

    <aside class="doc-page__toc">
      <div class="doc-page__toc-inner">
        <!-- Mounted with the article, so the observer has headings to attach to. -->
        <DocToc v-if="html" :headings="doc.headings" :container="articleEl" />
      </div>
    </aside>
  </div>

  <div v-else class="doc-page__missing">
    <h1>No such document</h1>
    <p>
      There is no <code>{{ slug }}</code> in the corpus. Pick one from the list.
    </p>
  </div>

  <ImageLightbox ref="lightbox" />
</template>

<style scoped lang="scss">
.doc-page {
  display: grid;
  grid-template-columns: 1fr;
  gap: var(--space-6);

  @include from('xl') {
    grid-template-columns: minmax(0, 1fr) var(--width-docs-toc);
    gap: var(--space-8);
  }

  &__main {
    min-width: 0;
  }

  &__head {
    margin-bottom: var(--space-6);
    padding-bottom: var(--space-5);
    border-bottom: var(--border-hair) solid var(--c-border);
  }

  &__title {
    font-size: var(--text-3xl);
  }

  &__lead {
    max-width: var(--measure-prose);
    margin-top: var(--space-3);
    color: var(--c-text-muted);
    font-size: var(--text-lg);
    line-height: var(--leading-relaxed);
  }

  &__loading {
    padding-block: var(--space-6);
    color: var(--c-text-faint);
  }

  &__toc {
    display: none;

    @include from('xl') {
      display: block;
    }
  }

  &__toc-inner {
    position: sticky;
    top: calc(var(--header-height) + var(--space-5));
    max-height: calc(100dvh - var(--header-height) - var(--space-8));
    overflow-y: auto;
    @include thin-scrollbar;
  }

  &__missing {
    padding-block: var(--space-8);

    p {
      margin-top: var(--space-3);
      color: var(--c-text-muted);
    }
  }
}
</style>
