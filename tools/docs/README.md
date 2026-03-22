# Lesma Docs

This directory contains the Astro Starlight documentation site for Lesma.

## Commands

Run all commands from `tools/docs`:

- `bun install` installs dependencies.
- `bun run dev` starts the local dev server.
- `bun run build` creates the production site in `dist/`.
- `bun run preview` previews the built site locally.

## Structure

- `src/content/docs/` holds the documentation pages.
- `src/assets/` holds site assets such as the Lesma logo.
- `public/` holds static files such as `CNAME`.
- `astro.config.mjs` defines the site metadata and sidebar.

## Deployment

GitHub workflows in the repository root build and deploy this site from
`tools/docs`.
