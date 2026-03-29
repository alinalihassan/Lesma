/**
 * True when the playground UI runs inside an iframe (e.g. docs `/playground` embed).
 */
export function isEmbeddedInParentFrame(): boolean {
  if (typeof window === 'undefined') {
    return false
  }
  try {
    return window.self !== window.top
  } catch {
    return true
  }
}
