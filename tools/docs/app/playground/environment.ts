/**
 * Optional `VITE_API_ORIGIN`: HTTP origin for the playground API (e.g. `https://lesma.dev`).
 * Defaults to the current page origin. LSP WebSocket URL is derived from this base.
 */
const fromEnv = import.meta.env.VITE_API_ORIGIN?.trim()
const rawApiBase = (fromEnv ? fromEnv.replace(/\/$/, '') : '') || window.location.origin

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
