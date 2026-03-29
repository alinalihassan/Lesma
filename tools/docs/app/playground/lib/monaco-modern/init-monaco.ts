import { init } from 'modern-monaco'

import { registerLesmaMonarchLanguage } from './lesma-monarch'

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
      defaultTheme: 'one-light',
      themes: ['one-light', 'one-dark-pro'],
      langs: ['json'],
    }).then((monaco) => {
      registerLesmaMonarchLanguage(monaco)
      return monaco
    })
  }
  return initPromise
}

export function applyMonacoColorScheme(monaco: MonacoApi, scheme: 'light' | 'dark'): void {
  monaco.editor.setTheme(scheme === 'dark' ? 'one-dark-pro' : 'one-light')
}
