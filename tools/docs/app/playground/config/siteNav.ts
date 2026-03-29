/**
 * Base URL for the documentation site (Fumadocs). Used for the shared top bar links.
 *
 * - Set `VITE_DOCS_URL` at build time when docs and playground are on different origins.
 * - In dev, defaults to the usual docs Vite port (see `tools/docs` `package.json` dev script).
 * - In production without `VITE_DOCS_URL`, falls back to `window.location.origin` (same-host deploy).
 */
export function docsSiteBaseUrl(): string {
  const fromEnv = import.meta.env.VITE_DOCS_URL?.trim()
  if (fromEnv) {
    return fromEnv.replace(/\/$/, '')
  }
  if (typeof window !== 'undefined') {
    return window.location.origin.replace(/\/$/, '')
  }
  return ''
}

export function githubRepoUrl(): string {
  const fromEnv = import.meta.env.VITE_GITHUB_URL?.trim()
  if (fromEnv) {
    return fromEnv.replace(/\/$/, '')
  }
  return 'https://github.com/alinalihassan/Lesma'
}
