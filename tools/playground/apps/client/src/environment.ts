/**
 * Global environment variables
 */
const rawApiBase = import.meta.env.VITE_LANG_SERVER ?? window.location.origin

const environment = {
  appVersion: import.meta.env.VITE_VERSION ?? '1.0.0-snapshot',
  apiUrl: rawApiBase,

  /** WebSocket URL for lesma-lsp (bridged by the playground server). */
  get lspWebSocketUrl(): string {
    try {
      const u = new URL(rawApiBase, window.location.href)
      const wsProto = u.protocol === 'https:' ? 'wss:' : 'ws:'
      return `${wsProto}//${u.host}/api/v2/lsp`
    } catch {
      const wsProto = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
      return `${wsProto}//${window.location.host}/api/v2/lsp`
    }
  },

  urls: {
    github: import.meta.env.VITE_GITHUB_URL ?? 'https://github.com/alinalihassan/Lesma',
    issue:
      import.meta.env.VITE_GITHUB_URL ?? 'https://github.com/alinalihassan/Lesma/issues/new',
  },
}

export default environment
