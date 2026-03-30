/**
 * True when the playground UI runs inside an iframe (e.g. a third-party embed or nested frame).
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
