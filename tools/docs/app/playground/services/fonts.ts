/// //////////////////////////////////////
// Monaco editor font registry (Fontsource) //
/// //////////////////////////////////////

/**
 * Default: stack only (no webfont until user picks one).
 */
export const DEFAULT_FONT = 'default'

export const fallbackFonts = [
  'Menlo',
  'Monaco',
  'Consolas',
  `"Lucida Console"`,
  `"Roboto Mono"`,
  `"Courier New"`,
  'monospace',
].join(',')

interface FontEntry {
  label: string
  /** Must match `font-family` from the corresponding @fontsource package CSS. */
  cssFamily: string
}

/**
 * Keys are persisted in localStorage (monaco.fontFamily).
 * `Hack` was replaced by Roboto Mono; `Zed-Mono` by Geist Mono (not on Fontsource).
 */
const fontRegistry: Record<string, FontEntry> = {
  CascadiaCode: { label: 'Cascadia Code', cssFamily: 'Cascadia Code' },
  'Comic-Mono': { label: 'Comic Mono', cssFamily: 'Comic Mono' },
  FiraCode: { label: 'Fira Code', cssFamily: 'Fira Code' },
  'IBM-Plex': { label: 'IBM Plex Mono', cssFamily: 'IBM Plex Mono' },
  'JetBrains-Mono': { label: 'JetBrains Mono', cssFamily: 'JetBrains Mono' },
  'Roboto-Mono': { label: 'Roboto Mono', cssFamily: 'Roboto Mono' },
  'Geist-Mono': { label: 'Geist Mono', cssFamily: 'Geist Mono' },
}

/**
 * Returns default monospace font style
 */
export const getDefaultFontFamily = () => fallbackFonts

/**
 * Legacy no-op: all fonts are loaded via `~/fonts/fontsource` imports.
 */
export function loadFont(_fontName: string): void {}

/**
 * Resolves editor font-family CSS. Unknown keys fall back to system monospace.
 */
export function getFontFamily(fontName: string): string {
  if (fontName === DEFAULT_FONT) {
    return fallbackFonts
  }
  const entry = fontRegistry[fontName]
  if (!entry) {
    if (fontName === 'Hack' || fontName === 'Zed-Mono') {
      console.warn(
        'getFontFamily: font "%s" was removed; choose another font in Settings. Using system monospace.',
        fontName,
      )
    } else {
      console.warn('getFontFamily: unknown font "%s", fallback monospace font used', fontName)
    }
    return fallbackFonts
  }

  return `'${entry.cssFamily}',${fallbackFonts}`
}

export const getAvailableFonts = () =>
  Object.keys(fontRegistry).map((family) => ({
    family,
    label: fontRegistry[family].label,
  }))
