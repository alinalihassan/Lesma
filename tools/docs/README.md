# Lesma documentation site

Vite + React Router + **Fumadocs** for [lesma-lang.com](https://lesma-lang.com/).

The playground UI lives in this app at [`/playground`](./app/routes/playground.tsx) (same SPA as the docs, lazy-loaded). The **site header** (logo, Documentation, Playground, search, theme) is shared with the rest of the site via Fumadocs `HomeLayout` + [`HomeSiteHeader`](./app/components/site-header.tsx); the playground only adds its own **toolbar** (Run, Examples, Settings) and status bar.

### Shipping

The Vite build (`bun run build`) is only **browser assets**. Production ships **`tools/playground/Dockerfile`** (or `bun run dev` / `wrangler deploy` there): one process serves `build/client` **and** **`/api`** (`lesma run`, compile, `lesma-lsp`). Use **`tools/playground/wrangler.jsonc`** on Cloudflare (Workers + **Containers**).

## Commands

From `tools/docs`:

- `bun install` — installs dependencies and runs `fumadocs-mdx` (generates `.source/`).
- `bun run dev` — dev server on **port 5174**. Run the Bun API from `tools/playground` (`bun run dev`, default **8080**) so `/api` and LSP WebSockets work; Vite proxies `/api` to that server (see [`vite.config.ts`](./vite.config.ts)).
- `bun run build` — production client build under `build/client/` (prerendered HTML + SPA assets).
- `bun run start` — local-only: serve `build/client` with `serve` (no `/api`; use for a quick static preview of the built SPA).
- `bun run types:check` — React Router typegen, MDX codegen, and `tsc`.
- **`bun run deploy:cloudflare`** — runs **`deploy:cloudflare`** in `tools/playground` (Workers + **Containers** with the unified image).
- **`bun run dev:cloudflare`** — `wrangler dev` from `tools/playground`.

## Cloudflare

Do **not** deploy this folder alone as a static-only Worker for the main site. Use **`tools/playground/wrangler.jsonc`**, which runs the Docker image that serves:

- docs + playground SPA at `/` (playground at `/playground`)
- compiler API + LSP at `/api`

1. `cd tools/playground && bun install`
2. `npx wrangler login`
3. From `tools/docs`: `bun run deploy:cloudflare` (delegates to `tools/playground`), or run `bun run deploy:cloudflare` directly in `tools/playground`.

## Structure

- `app/` — React Router routes, layouts, search dialog.
- `content/docs/` — MDX documentation and `meta.json` sidebars.
- `public/` — static assets (`CNAME`, favicon, logo).
- `serve.json` — SPA rewrite rules for `bun run start` (local preview).

## Editing on GitHub

“View options” / source links use paths under `tools/docs/content/docs/` in the Lesma repository.
