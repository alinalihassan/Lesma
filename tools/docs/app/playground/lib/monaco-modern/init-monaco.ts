import { init } from 'modern-monaco'

import { registerLesmaMonarchLanguage } from './lesma-monarch'

/** Shiki theme when the site / editor is in light mode. */
export const PLAYGROUND_SHIKI_LIGHT_THEME = 'one-light' as const

/** Shiki theme when the site / editor is in dark mode. */
export const PLAYGROUND_SHIKI_DARK_THEME = 'dark-plus' as const

const BUNDLED_SHIKI_THEMES = [PLAYGROUND_SHIKI_LIGHT_THEME, PLAYGROUND_SHIKI_DARK_THEME] as const

/** Runtime API object returned by `init()` (Monaco + Shiki wiring). */
export type MonacoApi = typeof import('modern-monaco/editor-core')

let initPromise: Promise<MonacoApi> | null = null

/**
 * One-time [modern-monaco](https://github.com/esm-dev/modern-monaco) bootstrap (Shiki + editor core).
 * Lesma uses a Monarch tokenizer — custom TextMate grammars in `init({ langs })` are not hooked to Monaco
 * by modern-monaco (only built-in tm-grammars get `setTokensProvider`).
 */
export function ensureMonaco(): Promise<MonacoApi> {
  if (!initPromise) {
    initPromise = init({
      defaultTheme: PLAYGROUND_SHIKI_LIGHT_THEME,
      themes: [...BUNDLED_SHIKI_THEMES],
      langs: ['json'],
    }).then((monaco) => {
      registerLesmaMonarchLanguage(monaco)
      return monaco
    })
  }
  return initPromise
}

export function applyMonacoColorScheme(monaco: MonacoApi, scheme: 'light' | 'dark'): void {
  const name = scheme === 'dark' ? PLAYGROUND_SHIKI_DARK_THEME : PLAYGROUND_SHIKI_LIGHT_THEME
  monaco.editor.setTheme(name)
}
