# web

The site at [cdj3k-mods.com](https://cdj3k-mods.com). Vue 3 + Vite, no SSR.

```bash
cd web && npm install && npm run dev
```

Then open <http://localhost:5173/>. The dev server uses the same base path as
the deployed site.

|                     |                                     |
| ------------------- | ----------------------------------- |
| `npm run dev`       | dev server, with the corpus watched |
| `npm run build`     | typecheck, then build to `web/dist` |
| `npm run preview`   | serve the built output              |
| `npm run typecheck` | `vue-tsc` on its own                |

`BASE_PATH=/cdj3k-mods/ npm run build` builds for a GitHub project page instead
of a domain root.

## How it fits together

The markdown in `../docs/` is read, rendered and indexed **at build time** by
`build/docs-plugin.ts`, which exposes it as three virtual modules: a manifest
that every page loads, one chunk per page, and the search index. The browser
never sees a markdown parser or a syntax highlighter.

Adding a page is dropping a `.md` into `../docs/`. It is picked up
automatically; naming it in `src/content/docs.ts` only decides where it sits
in the reading order. Cross-page links are written as plain
`other-page.md#anchor` and rewritten to routes at build time. A link to a page
that does not exist is unwrapped to plain text rather than left dead.

Screenshots go in `public/img/` and are referenced from the markdown as
`img/name.png`; the base path is applied at build time.

Two rules the codebase tries to keep:

- **One fact, one place.** The documentation says how to use something. The feature
  list (`src/content/features.ts`) is a catalogue: one line each, plus where it
  is switched, and links into the documentation. Neither restates the other.
- **Tokens, not values.** Every colour, space, size and duration is a custom
  property in `src/styles/tokens/`. A component that needs a raw hex is a token
  that is missing.

## Deploying

`.github/workflows/pages.yml` builds and publishes to GitHub Pages on a push to
`main` that touches `web/` or `docs/`. Pages is set to **Source: GitHub
Actions**, and `public/CNAME` carries the custom domain into the artifact.
