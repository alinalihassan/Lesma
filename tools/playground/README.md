# Lesma playground (Bun + Hono + Vite)

Monorepo under **Bun workspaces** (`apps/*`):

- **`apps/server`** — [Hono](https://hono.dev/) on Bun: HTTP API (`/api/*`), static SPA, WebSocket bridge to `lesma-lsp`.
- **`apps/client`** — [Vite](https://vitejs.dev/) + React + [modern-monaco](https://github.com/esm-dev/modern-monaco) (Monaco + Shiki).

## Prerequisites

- [Bun](https://bun.sh/) 1.x only (no npm/yarn for installs).

## Setup & local development

From **`tools/playground`**:

```bash
bun install
```

API (Bun):

```bash
bun run dev
```

UI (Vite, separate terminal):

```bash
bun run dev:client
```

Set `LISTEN_ADDR` / `VITE_API_PROXY` if the API is not on the default `127.0.0.1:8080` (see `apps/client/vite.config.ts`).

The client **site nav** (Lesma logo, Documentation, Playground, Search, theme, GitHub) matches the docs top bar. **`VITE_DOCS_URL`** — optional at build time; in dev it defaults to `http://localhost:5174` (the docs Vite server). In production builds without it, links use `window.location.origin` (same-host deploy). For a separate docs origin, set e.g. `VITE_DOCS_URL=https://example.com` when running `vite build` or pass Docker build-arg `VITE_DOCS_URL`.

## Production build

**Playground only** (no docs):

```bash
bun run build
bun run start -- --addr=:8080 --static-dir=apps/client/build
```

**Unified docs + playground** (same as the Cloudflare container): build the docs app into `tools/docs/build/client`, build the client with `PLAYGROUND_BASE=/playground/`, then run the server with a docs static dir:

```bash
# from repo root — example local smoke test
(cd tools/docs && bun install && bun run build)
PLAYGROUND_BASE=/playground/ bun run --cwd tools/playground build:client
bun run --cwd tools/playground/apps/server start -- \
  --addr=:8080 \
  --static-dir=tools/playground/apps/client/build \
  --docs-static-dir=tools/docs/build/client
```

The **`tools/playground/Dockerfile`** automates Lesma + docs + playground client and sets `DOCS_ASSETS_DIR` + `PLAYGROUND_BASE=/playground/`.

## Typecheck

Server:

```bash
bun run typecheck
```

Client:

```bash
bun run typecheck:client
```

## Docker

From the **repository root** (with `vcpkg` initialized):

```bash
docker build -f tools/playground/Dockerfile -t lesma-web .
```

## Cloudflare (Workers + Containers)

Deploy the same Docker image behind a Worker using **`wrangler.jsonc`** and **`worker.ts`** in this directory (`image`: `./Dockerfile`, `image_build_context`: repo root `../..`).

Prerequisites: Cloudflare **Workers** and **Containers**, Docker (linux/amd64 for deploy — on Apple Silicon e.g. `export DOCKER_DEFAULT_PLATFORM=linux/amd64`), and submodules so the Dockerfile can build Lesma.

```bash
bun install
npx wrangler login
bun run deploy:cloudflare
```

Local: `bun run dev:cloudflare` (Wrangler [container dev](https://developers.cloudflare.com/containers/local-dev/) when Docker is available). Set `enable_containers: false` under `dev` in `wrangler.jsonc` to iterate on the Worker only.

The same Worker URL serves **docs and playground**; you normally do **not** set `VITE_PLAYGROUND_URL`. Set it only if the playground stays on a **separate** origin (then enable CORS on that API if needed).

## Environment (server)

See `apps/server/src/config.ts` for flags and env vars (`LESMA_BIN`, `LESMA_LSP_BIN`, `LESMA_RUN_TIMEOUT`, etc.).

## Troubleshooting

- **`EADDRINUSE` on port 8080** — Free the port or use e.g. `LISTEN_ADDR=:8787 bun dev` and match Vite: `LISTEN_ADDR=:8787 bun run dev:client` or `VITE_API_PROXY=http://127.0.0.1:8787 bun run dev:client`.
- **API / LSP from the Vite app** — Defaults proxy `/api` (and WebSockets) to **8080**; see `vite.config.ts`.
