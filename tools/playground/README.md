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

## Production build

```bash
bun run build
bun run start -- --addr=:8080 --static-dir=apps/client/build
```

## Typecheck

Server:

```bash
bun run typecheck
```

Client (optional; may report existing strictness issues):

```bash
bun run typecheck:client
```

## Docker

From the **repository root** (with `vcpkg` initialized):

```bash
docker build -f tools/playground/Dockerfile -t lesma-playground .
```

## Environment (server)

See `apps/server/src/config.ts` for flags and env vars (`LESMA_BIN`, `LESMA_LSP_BIN`, `LESMA_RUN_TIMEOUT`, etc.).

## Troubleshooting

- **`EADDRINUSE` on port 8080** — Free the port or use e.g. `LISTEN_ADDR=:8787 bun dev` and match Vite: `LISTEN_ADDR=:8787 bun run dev:client` or `VITE_API_PROXY=http://127.0.0.1:8787 bun run dev:client`.
- **API / LSP from the Vite app** — Defaults proxy `/api` (and WebSockets) to **8080**; see `vite.config.ts`.
