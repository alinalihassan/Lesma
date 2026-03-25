/**
 * Global environment variables
 */
const rawApiBase = import.meta.env.VITE_LANG_SERVER ?? window.location.origin

const environment = {
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
}

export default environment
