import type { TextmateGrammar } from 'modern-monaco'
import { registerSyntax } from 'modern-monaco/core'
import { init } from 'modern-monaco'

import { lesmaTextmateGrammar } from '../../../../lib/lesma-textmate-grammar'

import { applyLesmaLanguageConfiguration } from './lesma-language-configuration'

/** Shiki theme when the site / editor is in light mode. */
export const PLAYGROUND_SHIKI_LIGHT_THEME = 'one-light' as const

/** Shiki theme when the site / editor is in dark mode. */
export const PLAYGROUND_SHIKI_DARK_THEME = 'dark-plus' as const

const BUNDLED_SHIKI_THEMES = [PLAYGROUND_SHIKI_LIGHT_THEME, PLAYGROUND_SHIKI_DARK_THEME] as const

registerSyntax(lesmaTextmateGrammar as TextmateGrammar)

/** Runtime API object returned by `init()` (Monaco + Shiki wiring). */
export type MonacoApi = typeof import('modern-monaco/editor-core')

let initPromise: Promise<MonacoApi> | null = null

/**
 * One-time [modern-monaco](https://github.com/esm-dev/modern-monaco) bootstrap (Shiki + editor core).
 * Lesma highlighting uses the same TextMate grammar as the VS Code extension (`tools/vscode/syntaxes`).
 */
export function ensureMonaco(): Promise<MonacoApi> {
  if (!initPromise) {
    initPromise = init({
      defaultTheme: PLAYGROUND_SHIKI_LIGHT_THEME,
      themes: [...BUNDLED_SHIKI_THEMES],
      langs: ['json'],
    }).then((monaco) => {
      applyLesmaLanguageConfiguration(monaco)
      return monaco
    })
  }
  return initPromise
}

export function applyMonacoColorScheme(monaco: MonacoApi, scheme: 'light' | 'dark'): void {
  const name = scheme === 'dark' ? PLAYGROUND_SHIKI_DARK_THEME : PLAYGROUND_SHIKI_LIGHT_THEME
  monaco.editor.setTheme(name)
}
