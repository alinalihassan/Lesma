import markdownit from 'markdown-it'
import type { MarkedString, MarkupContent } from 'vscode-languageserver-protocol'
import type { DocContent } from '../types/autocomplete'

import { classNames } from './styles'

const MD_CODE_BLOCK = '```'

interface NormalizedContent {
  isMarkdown: boolean
  value: string
}

type MarkupEntry = MarkedString | MarkupContent

const normalizeMarkupEntry = (entry: MarkupEntry): NormalizedContent => {
  if (typeof entry === 'string') {
    return {
      isMarkdown: false,
      value: entry,
    }
  }

  if ('kind' in entry) {
    return normalizeMarkupContentValue(entry)
  }

  const lang = entry.language ?? ''
  const value = `${MD_CODE_BLOCK}${lang}\n${entry.value}\n${MD_CODE_BLOCK}`

  return {
    isMarkdown: true,
    value,
  }
}

const normalizeMarkupContentValue = (content: MarkupContent): NormalizedContent => {
  return {
    isMarkdown: content.kind === 'markdown',
    value: content.value,
  }
}

const normalizeMarkupContent = (content: DocContent): NormalizedContent => {
  if (Array.isArray(content)) {
    return content.reduce(
      (acc: NormalizedContent, item: MarkupEntry): NormalizedContent => {
        const normalizedItem = normalizeMarkupEntry(item)
        return {
          isMarkdown: acc.isMarkdown || normalizedItem.isMarkdown,
          value: acc.value ? acc.value + '\n\n' + normalizedItem.value : normalizedItem.value,
        }
      },
      { isMarkdown: true, value: '' },
    )
  }

  return normalizeMarkupEntry(content)
}

export class MarkupRenderer {
  private readonly printer: markdownit
  constructor() {
    this.printer = markdownit({
      html: false,
      breaks: true,
      highlight: (str: string, _lang: string, _attrs: string) => {
        return `<pre class="code">${str}</pre>`
      },
    })
  }

  renderContents(dst: HTMLElement, contents: DocContent) {
    const { isMarkdown, value } = normalizeMarkupContent(contents)
    if (isMarkdown) {
      dst.innerHTML = this.printer.render(value)
      return
    }

    const pre = document.createElement('pre')
    pre.innerText = value
    pre.style.whiteSpace = 'pre-wrap'
    dst.appendChild(pre)
  }
}

export const renderCompletionDoc = (renderer: MarkupRenderer, doc?: DocContent) => {
  if (!doc) {
    return null
  }

  const node = document.createElement('div')
  node.classList.add(classNames.completionDoc)
  if (typeof doc === 'string') {
    node.innerText = doc
  } else {
    renderer.renderContents(node, doc)
  }

  return node
}
