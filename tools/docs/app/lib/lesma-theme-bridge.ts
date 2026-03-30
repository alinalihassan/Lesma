/**
 * Bridges Fumadocs / `next-themes` (docs chrome) with the playground Redux theme toggle.
 * Registered from {@link LesmaThemeBridge} inside `RootProvider`.
 */

/** Default `next-themes` storage key (keep in sync with `RootProvider` `theme.storageKey`). */
export const LESMA_NEXT_THEME_STORAGE_KEY = 'theme';

export type LesmaChromeTheme = 'light' | 'dark';

let setThemeImpl: ((theme: LesmaChromeTheme) => void) | null = null;

export function registerLesmaThemeSetter(fn: ((t: LesmaChromeTheme) => void) | null): void {
  setThemeImpl = fn;
}

export function setLesmaChromeTheme(theme: LesmaChromeTheme): void {
  setThemeImpl?.(theme);
}

/** Read persisted chrome theme before Redux hydrates (SSR-safe). */
export function readStoredChromeThemeIsDark(): boolean | null {
  if (typeof window === 'undefined') {
    return null;
  }
  try {
    const t = localStorage.getItem(LESMA_NEXT_THEME_STORAGE_KEY);
    if (t === 'dark') {
      return true;
    }
    if (t === 'light') {
      return false;
    }
  } catch {
    /* ignore */
  }
  return null;
}
