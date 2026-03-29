# Lesma playground (Bun + Hono)

Single package: **`src/`** is a [Hono](https://hono.dev/) server on Bun — HTTP API (`/api/*`), static SPA from the docs build, WebSocket bridge to `lesma-lsp`. **`worker.ts`** is the Cloudflare Worker entry (containers).

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
- **`VITE_API_PROXY`** / `LISTEN_ADDR` — set if the API is not on the default `127.0.0.1:8080` (see `src/config.ts` and `tools/docs/vite.config.ts`).
- **`VITE_API_ORIGIN`** (optional, docs Vite build) — only if the browser must call an API on another origin; defaults to `window.location.origin`.

**Shipping:** static `build/client` alone is not enough for Run/LSP — you need this Bun process (or the Docker image). See **`tools/docs/README.md`** (*Shipping*).

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

**CI:** GitHub Actions workflow **Deploy Cloudflare** runs `wrangler deploy` on every push to **`main`** or **`dev`**. Add repository secrets `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID`. Each run builds Lesma inside Docker and can take on the order of tens of minutes on a default runner.

The same Worker URL serves **docs and playground** on one origin.

### Custom domain (e.g. lesma.dev)

One hostname is enough: **docs** are at `https://lesma.dev/`, the **playground** at `https://lesma.dev/playground`, and the **compiler API + LSP** at `https://lesma.dev/api` (same origin, so no extra CORS setup).

1. Add **`lesma.dev`** to your Cloudflare account as a zone (change its nameservers at the registrar to Cloudflare’s, or use a DNS-only setup that still delegates the zone to Cloudflare for Workers).
2. **`wrangler.jsonc`** lists bare hostnames `lesma.dev` and `www.lesma.dev` with `custom_domain: true` (no `/*` — Cloudflare custom domains are hostname-only). Adjust or remove hostnames to match what you own.
3. Run **`bun run deploy:cloudflare`**. Wrangler attaches those custom domains to this Worker; Cloudflare will show any DNS records still needed under the zone (often auto-managed when the zone is on Cloudflare).
4. Optional: add a **Redirect rule** in the dashboard (`www.lesma.dev/*` → `https://lesma.dev/$1`) if you only want the apex as canonical.

## Environment (server)

See `src/config.ts` for flags and env vars (`LESMA_BIN`, `LESMA_LSP_BIN`, `LESMA_RUN_TIMEOUT`, etc.).

## Troubleshooting

- **`EADDRINUSE` on port 8080** — Free the port or use e.g. `LISTEN_ADDR=:8787 bun dev` and set `VITE_API_PROXY=http://127.0.0.1:8787` when running `tools/docs` Vite dev. Only if the client must target a different API host, set **`VITE_API_ORIGIN`** on the docs build.
- **API / LSP from the Vite app** — Defaults proxy `/api` (and WebSockets) to **8080**; see `tools/docs/vite.config.ts`.
