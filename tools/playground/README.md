# Lesma playground (Bun + Hono)

Bun workspace with **`apps/server`** only:

- **`apps/server`** — [Hono](https://hono.dev/) on Bun: HTTP API (`/api/*`), static SPA from the docs build, WebSocket bridge to `lesma-lsp`.

The playground **UI** (Monaco, terminal, Redux) is part of **`tools/docs`** (`app/playground/`) and ships as the `/playground` route in the same Vite bundle as the docs.

## Prerequisites

- [Bun](https://bun.sh/) 1.x only (no npm/yarn for installs).

## Setup & local development

From **`tools/playground`**:

```bash
bun install
```

API + static hosting (Bun):

```bash
bun run dev
```

For a full local loop, build or run the docs app and point the server at its client output, **or** use Vite dev with proxying:

- **Recommended dev:** from `tools/docs`, `bun run dev` (port **5174**) and, in another terminal, `bun run dev` here so `/api` is proxied by Vite to **8080** (see `tools/docs/vite.config.ts`).
- **`VITE_API_PROXY`** / `LISTEN_ADDR` — set if the API is not on the default `127.0.0.1:8080` (see `apps/server/src/config.ts` and `tools/docs/vite.config.ts`).

Playground in-app links use **`VITE_DOCS_URL`** when set at docs build time; otherwise `window.location.origin` (same-host deploy).

**Shipping:** static `build/client` alone is not enough for Run/LSP — you need this Bun process (or the Docker image). See **`tools/docs/README.md`** (section *Shipping (static files vs server)*).

## Production build

Serve the **unified** static tree produced by the docs app:

```bash
(cd tools/docs && bun install && bun run build)
bun run start -- --addr=:8080 --static-dir=../docs/build/client
```

The **`tools/playground/Dockerfile`** builds `tools/docs` and copies `build/client` into the image; see that file for the exact layout.

## Typecheck

```bash
bun run typecheck
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

- **`EADDRINUSE` on port 8080** — Free the port or use e.g. `LISTEN_ADDR=:8787 bun dev` and set `VITE_API_PROXY=http://127.0.0.1:8787` when running `tools/docs` Vite dev.
- **API / LSP from the Vite app** — Defaults proxy `/api` (and WebSockets) to **8080**; see `tools/docs/vite.config.ts`.
