/// <reference types="vite/client" />
/// <reference types="vite-plugin-svgr/client" />

interface ImportMetaEnv {
  readonly VITE_VERSION: string
  readonly VITE_LANG_SERVER: string
  /** Optional; defaults to docs dev server in `import.meta.env.DEV`, else `window.location.origin`. */
  readonly VITE_DOCS_URL?: string
  readonly VITE_GITHUB_URL?: string
  readonly BASE_URL: string
}

interface ImportMeta {
  readonly env: ImportMetaEnv
}
