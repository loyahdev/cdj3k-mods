/*
 * Which heading the reader is currently under, for the table of contents.
 *
 * IntersectionObserver, not a scroll handler: nothing here reads scroll
 * position, animates on it, or moves anything as the page moves. The only
 * effect is which entry in the contents list is marked current, which is
 * navigation state -- the same thing a router-link active class is.
 */
import { ref, onMounted, onBeforeUnmount, watch, type Ref } from 'vue'

export function useActiveHeading(container: Ref<HTMLElement | null>, ids: Ref<string[]>) {
  const active = ref<string>('')
  let observer: IntersectionObserver | null = null

  /* Headings currently on screen, in document order. The topmost of them is
   * the one the reader is under; when none are (a long section between two
   * headings) the last one to leave upwards stays current. */
  const visible = new Set<string>()

  function pick() {
    for (const id of ids.value) {
      if (visible.has(id)) {
        active.value = id
        return
      }
    }
  }

  function connect() {
    observer?.disconnect()
    if (!container.value) return

    visible.clear()
    observer = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          const id = entry.target.id
          if (entry.isIntersecting) visible.add(id)
          else {
            visible.delete(id)
            // Left upwards: everything below it is still ahead of the reader.
            if (entry.boundingClientRect.top < 0) active.value = id
          }
        }
        pick()
      },
      {
        /* A band just under the sticky header. Bottom margin pulled well up so
         * a heading only counts once it has actually reached reading position. */
        rootMargin: '-72px 0px -70% 0px',
        threshold: 0,
      },
    )

    for (const id of ids.value) {
      const el = container.value.querySelector(`#${CSS.escape(id)}`)
      if (el) observer.observe(el)
    }

    active.value = ids.value[0] ?? ''
  }

  onMounted(connect)
  // 'post': the headings have to be in the DOM before anything can observe them.
  watch([container, ids], connect, { flush: 'post' })
  onBeforeUnmount(() => observer?.disconnect())

  return { active }
}
