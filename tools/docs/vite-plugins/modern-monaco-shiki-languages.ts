import type { Plugin } from 'vite'

/**
 * modern-monaco only registers Shiki → Monaco tokenizers for grammars in its bundled list.
 * Custom grammars added via `registerSyntax()` are loaded into Shiki but never get
 * `registerShikiMonacoTokenizer`, so merge `highlighter.getLoadedLanguages()` into the set.
 */
export function modernMonacoShikiLoadedLanguages(): Plugin {
  const needle =
    'const allLanguages = new Set(grammars.filter((g) => !g.injectTo).map((g) => g.name));'
  const replacement = `${needle}
  for (const id of highlighter.getLoadedLanguages()) {
    allLanguages.add(id);
  }`

  return {
    name: 'modern-monaco-shiki-loaded-languages',
    transform(code, id) {
      const normalizedId = id.replace(/\\/g, '/')
      if (normalizedId.includes('modern-monaco/dist/core.mjs') && code.includes(needle)) {
        return code.replace(needle, replacement)
      }
      return null
    },
  }
}
