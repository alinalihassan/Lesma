import Anser from 'anser'

/** ESC (U+001B) or legacy Ctrl+[ — start of CSI / other terminal escapes. */
export function containsAnsiEscapes(text: string): boolean {
  return /[\u001b\x1b]/.test(text)
}

/**
 * SGR-colored HTML for the fallback output panel. Call only when {@link containsAnsiEscapes} is true.
 */
export function ansiToPlaygroundHtml(text: string): string {
  if (!text) {
    return ''
  }
  return Anser.ansiToHtml(text, { use_classes: false })
}
