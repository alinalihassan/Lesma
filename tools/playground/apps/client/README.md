# Lesma Playground (web UI)

React + Vite client for the in-repo playground: editor, LSP bridge, run output, and examples.

## Run in development

From `tools/playground/`:

```bash
bun install
bun run dev
```

The Vite dev server defaults to `http://localhost:3000` and proxies `/api` to the Bun server (see `vite.config.ts` and `VITE_API_PROXY`).

## Tech stack

- React 18 (`react`, `react-dom`)
- TypeScript 5
- Vite 5 with SWC (`@vitejs/plugin-react-swc`)
- Fluent UI (`@fluentui/react`)
- [modern-monaco](https://github.com/esm-dev/modern-monaco) (Monaco + Lesma TextMate grammar + WebSocket LSP bridge)
- State and routing: Redux + React Redux, `connected-react-router`, React Router v5
- Terminal: xterm.js (`@xterm/xterm` + addons)
- Tests: Vitest + `@testing-library/jest-dom` + JSDOM
- Lint/format: ESLint 9 + `typescript-eslint` + Prettier

## Environment variables

Build-time values use the `VITE_*` prefix where applicable. For local dev, the important one is `VITE_API_PROXY` (optional override for the `/api` proxy target); see `vite.config.ts`.
