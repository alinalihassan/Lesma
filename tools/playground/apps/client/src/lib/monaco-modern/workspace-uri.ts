/** LSP / Monaco model URI for a workspace-relative path (matches lesma-lsp + prior CodeMirror client). */
export function workspaceFileUri(relPath: string): string {
  const parts = relPath.split('/').map((seg) => encodeURIComponent(seg))
  return `file:///workspace/${parts.join('/')}`
}
