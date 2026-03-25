import snippets from './snippets.json'
import type { SnippetSource, Snippets } from './types'

export type { Snippet, SnippetSource } from './types'

const baseUrl = new URL(import.meta.env.BASE_URL, location.origin)
const snippetsBaseDir = 'examples'

export const getSnippetFromSource = async (
  source: SnippetSource,
  signal?: AbortSignal,
): Promise<Record<string, string>> => {
  const { basePath, files } = source
  const promises = files.map(async (file) => {
    const url = new URL(`${snippetsBaseDir}/${basePath}/${file}`, baseUrl)
    const rsp = await fetch(url, signal ? { signal } : undefined)
    if (!rsp.ok) throw new Error(`HTTP Error: ${rsp.status} ${rsp.statusText}`)
    return [file, await rsp.text()] as const
  })

  const results = await Promise.all(promises)
  return Object.fromEntries(results)
}

export const getSnippetsList = (): Snippets => snippets as Snippets
