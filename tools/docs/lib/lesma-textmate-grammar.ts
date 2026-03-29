import type { LanguageRegistration } from 'shiki'

import raw from '../../vscode/syntaxes/lesma.tmLanguage.json'

/**
 * Lesma grammar for Shiki / TextMate consumers. `name` must match fenced-block ids (`lesma`, `les` via alias).
 */
export const lesmaTextmateGrammar: LanguageRegistration = {
  ...(raw as LanguageRegistration),
  name: 'lesma',
}
