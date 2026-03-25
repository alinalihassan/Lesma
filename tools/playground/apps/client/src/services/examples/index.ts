import snippets from './snippets.json'
import type { Snippets } from './types'

export * from './types'
export * from './client'

export const getSnippetsList = () => snippets as Snippets
