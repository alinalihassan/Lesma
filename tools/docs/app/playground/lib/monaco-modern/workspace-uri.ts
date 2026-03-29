/** LSP / Monaco model URI for a workspace-relative path (matches lesma-lsp + prior CodeMirror client). */
export function workspaceFileUri(relPath: string): string {
  const parts = relPath.split('/').map((seg) => encodeURIComponent(seg))
  return `file:///workspace/${parts.join('/')}`
}

/** Inverse of {@link workspaceFileUri} for `textDocument/publishDiagnostics` URIs. */
export function workspacePathFromFileUri(uriStr: string): string | null {
  try {
    const u = new URL(uriStr)
    if (u.protocol !== 'file:') {
      return null
    }
    const prefix = '/workspace/'
    const { pathname } = u
    if (!pathname.startsWith(prefix)) {
      return null
    }
    const rest = pathname.slice(prefix.length)
    if (!rest) {
      return null
    }
    return rest
      .split('/')
      .map((seg) => decodeURIComponent(seg))
      .join('/')
  } catch {
    return null
  }
}
