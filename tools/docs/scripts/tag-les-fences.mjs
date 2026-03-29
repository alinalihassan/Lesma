#!/usr/bin/env node
/**
 * Turn bare ``` fences into ```les for Lesma samples. Leaves ```bash, ```sh, etc. unchanged.
 * Closing ``` lines are detected via in-block state.
 */
import { readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'

function* walkMdx(dir) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name)
    if (statSync(p).isDirectory()) yield* walkMdx(p)
    else if (name.endsWith('.mdx')) yield p
  }
}

function transform(content) {
  const lines = content.split('\n')
  let inBlock = false
  const out = lines.map((line) => {
    const t = line.trim()
    if (t === '```') {
      if (!inBlock) {
        inBlock = true
        const i = line.indexOf('`')
        const indent = i >= 0 ? line.slice(0, i) : ''
        return `${indent}\`\`\`les`
      }
      inBlock = false
      return line
    }
    if (t.startsWith('```') && t.length > 3) {
      inBlock = true
      return line
    }
    return line
  })
  return out.join('\n')
}

const root = join(fileURLToPath(new URL('.', import.meta.url)), '..', 'content', 'docs')
for (const p of walkMdx(root)) {
  const c = readFileSync(p, 'utf8')
  const n = transform(c)
  if (n !== c) writeFileSync(p, n)
}
