import { json } from '@codemirror/lang-json'
import { Compartment, type Extension } from '@codemirror/state'
import { Syntax } from '../types/common'

import { lesmaLanguage } from './lesma-syntax'

export { Syntax }

export const defaultSyntax = Syntax.Lesma

const getFileExtension = (fName?: string) => {
  const extPos = fName?.lastIndexOf('.') ?? -1
  return extPos !== -1 && fName?.toLowerCase()?.slice(extPos)
}

/**
 * Detects language for syntax highlight by extension in a file name.
 */
export const syntaxFromFileName = (fName?: string): Syntax => {
  switch (getFileExtension(fName)) {
    case '.les':
      return Syntax.Lesma
    case '.json':
      return Syntax.JSON
    default:
      return Syntax.PlainText
  }
}

const getSyntaxExtension = (lang: Syntax): Extension => {
  switch (lang) {
    case Syntax.JSON:
      return json()
    case Syntax.Lesma:
      return lesmaLanguage
    case Syntax.PlainText:
    default:
      return []
  }
}

export const syntaxCompartment = new Compartment()

/**
 * Returns a new syntax extension compartment with initial syntax highlighter based on a file name.
 */
export const newSyntaxCompartment = (fileName?: string) =>
  syntaxCompartment.of(getSyntaxExtension(syntaxFromFileName(fileName)))

/**
 * Returns a new EditorState effect to replace syntax highlight extension.
 *
 * Use `syntaxFromFileName` to get language.
 */
export const updateSyntaxEffect = (lang: Syntax) => syntaxCompartment.reconfigure(getSyntaxExtension(lang))
