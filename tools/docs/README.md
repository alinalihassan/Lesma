# Lesma documentation site

Vite + React Router + **Fumadocs** for [lesma-lang.com](https://lesma-lang.com/).

The playground UI is **embedded** from this app at [`/playground`](./app/routes/playground.tsx) (iframe). In production, both docs and the playground static bundle are served by the **unified** Bun server in `tools/playground` (see that README and `tools/playground/Dockerfile`).

## Commands

From `tools/docs`:

- `bun install` — installs dependencies and runs `fumadocs-mdx` (generates `.source/`).
- `bun run dev` — dev server on **port 5174**. The embed page loads the playground dev server at `http://localhost:3000` (run `bun run dev` + `bun run dev:client` under `tools/playground`).
- `bun run build` — production client build under `build/client/` (prerendered HTML + SPA assets).
- `bun run start` — serve `build/client` with `serve` and `serve.json` (SPA fallback) — docs only, no playground API.
- `bun run types:check` — React Router typegen, MDX codegen, and `tsc`.
- **`bun run deploy:cloudflare`** — runs **`deploy:cloudflare`** in `tools/playground` (Workers + **Containers** with the unified image).
- **`bun run dev:cloudflare`** — `wrangler dev` from `tools/playground`.

## Cloudflare

Do **not** deploy this folder alone as a static-only Worker for the main site. Use **`tools/playground/wrangler.jsonc`**, which runs the Docker image that serves:

- docs SPA at `/`
- playground SPA at `/playground/`
- compiler API + LSP at `/api`

1. `cd tools/playground && bun install`
2. `npx wrangler login`
3. From `tools/docs`: `bun run deploy:cloudflare` (delegates to `tools/playground`), or run `bun run deploy:cloudflare` directly in `tools/playground`.

## Environment

- **`VITE_PLAYGROUND_URL`** — optional. If set, the header “Playground” link and embed `iframe` use this origin instead of the in-app `/playground` page and `/playground/` static path. Use when the playground is still hosted separately.

## Structure

- `app/` — React Router routes, layouts, search dialog.
- `content/docs/` — MDX documentation and `meta.json` sidebars.
- `public/` — static assets (`CNAME`, favicon, logo).
- `serve.json` — SPA rewrite rules for static hosting (`bun run start`).

## Docker

- **Unified (recommended):** `docker build -f tools/playground/Dockerfile` from the repo root (includes docs + playground + Lesma binaries). See `tools/playground/README.md`.
- **Docs-only:** [`Dockerfile`](./Dockerfile) in this directory builds and serves static `build/client` (no playground API).

## Editing on GitHub

“View options” / source links use paths under `tools/docs/content/docs/` in the Lesma repository.
