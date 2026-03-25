import type { IMarkdownString, Position as MonacoPosition, editor, languages } from 'modern-monaco/editor-core'
import * as lsp from 'vscode-languageserver-protocol'

import type { MonacoApi } from './init-monaco'

type ITextModel = editor.ITextModel

function completionItemLabel(item: lsp.CompletionItem): string {
  const l = item.label as string | { label: string }
  return typeof l === 'string' ? l : l.label
}

function isLesmaWorkspaceUri(uri: string): boolean {
  try {
    const u = new URL(uri)
    return u.pathname.endsWith('.les')
  } catch {
    return uri.includes('.les')
  }
}

function lspRangeToMonacoRange(monaco: MonacoApi, r: lsp.Range) {
  return new monaco.Range(
    r.start.line + 1,
    r.start.character + 1,
    r.end.line + 1,
    r.end.character + 1,
  )
}

function toMarkerSeverity(monaco: MonacoApi, s?: lsp.DiagnosticSeverity) {
  switch (s) {
    case lsp.DiagnosticSeverity.Error:
      return monaco.MarkerSeverity.Error
    case lsp.DiagnosticSeverity.Warning:
      return monaco.MarkerSeverity.Warning
    case lsp.DiagnosticSeverity.Information:
      return monaco.MarkerSeverity.Info
    case lsp.DiagnosticSeverity.Hint:
      return monaco.MarkerSeverity.Hint
    default:
      return monaco.MarkerSeverity.Error
  }
}

function toCompletionItemKind(monaco: MonacoApi, k?: lsp.CompletionItemKind) {
  const M = monaco.languages.CompletionItemKind
  switch (k) {
    case lsp.CompletionItemKind.Method:
      return M.Method
    case lsp.CompletionItemKind.Function:
      return M.Function
    case lsp.CompletionItemKind.Constructor:
      return M.Constructor
    case lsp.CompletionItemKind.Field:
      return M.Field
    case lsp.CompletionItemKind.Variable:
      return M.Variable
    case lsp.CompletionItemKind.Class:
      return M.Class
    case lsp.CompletionItemKind.Interface:
      return M.Interface
    case lsp.CompletionItemKind.Module:
      return M.Module
    case lsp.CompletionItemKind.Property:
      return M.Property
    case lsp.CompletionItemKind.Unit:
      return M.Unit
    case lsp.CompletionItemKind.Value:
      return M.Value
    case lsp.CompletionItemKind.Enum:
      return M.Enum
    case lsp.CompletionItemKind.Keyword:
      return M.Keyword
    case lsp.CompletionItemKind.Snippet:
      return M.Snippet
    case lsp.CompletionItemKind.Color:
      return M.Color
    case lsp.CompletionItemKind.Reference:
      return M.Reference
    case lsp.CompletionItemKind.Folder:
      return M.Folder
    case lsp.CompletionItemKind.EnumMember:
      return M.EnumMember
    case lsp.CompletionItemKind.Constant:
      return M.Constant
    case lsp.CompletionItemKind.Struct:
      return M.Struct
    case lsp.CompletionItemKind.Event:
      return M.Event
    case lsp.CompletionItemKind.Operator:
      return M.Operator
    case lsp.CompletionItemKind.TypeParameter:
      return M.TypeParameter
    default:
      return M.Text
  }
}

function markupToMarkdownString(doc: lsp.MarkupContent | lsp.MarkedString | string): IMarkdownString {
  if (typeof doc === 'string') {
    return { value: doc }
  }
  if ('kind' in doc) {
    return { value: doc.value, isTrusted: true }
  }
  return { value: `\`\`\`${doc.language}\n${doc.value}\n\`\`\``, isTrusted: true }
}

/**
 * Minimal JSON-RPC LSP client for lesma-lsp: diagnostics, completion, hover (WebSocket, same transport as the old CodeMirror client).
 */
export class LesmaMonacoLspBridge {
  private ws: WebSocket | null = null
  private nextId = 0
  private readonly pending = new Map<
    number,
    { resolve: (v: unknown) => void; reject: (e: unknown) => void }
  >()
  private initialized = false
  private openUri: string | null = null
  private documentVersion = 0
  private flushTimer: ReturnType<typeof setTimeout> | null = null
  private documentDirty = false
  private modelSub: { dispose: () => void } | null = null
  private completionDisposable: { dispose: () => void } | null = null
  private hoverDisposable: { dispose: () => void } | null = null

  constructor(
    private readonly monaco: MonacoApi,
    private readonly socketUrl: string,
    private readonly getActiveModel: () => ITextModel | null,
  ) {}

  start(): void {
    this.registerLanguageFeatures()
    this.ws = new WebSocket(this.socketUrl)
    this.ws.binaryType = 'arraybuffer'
    this.ws.addEventListener('message', (ev: MessageEvent<string | ArrayBuffer>) => {
      const data = typeof ev.data === 'string' ? ev.data : new TextDecoder().decode(ev.data)
      this.handleMessage(data)
    })
    this.ws.addEventListener('open', () => {
      void this.handshake()
    })
    this.ws.addEventListener('error', () => {
      console.warn('[lesma-lsp] WebSocket error (is the playground server running with lesma-lsp?)', this.socketUrl)
    })
    this.ws.addEventListener('close', (ev) => {
      if (import.meta.env.DEV) {
        console.warn('[lesma-lsp] WebSocket closed', ev.code, ev.reason || '(no reason)')
      }
    })
  }

  private registerLanguageFeatures(): void {
    this.completionDisposable?.dispose()
    this.hoverDisposable?.dispose()

    this.completionDisposable = this.monaco.languages.registerCompletionItemProvider('lesma', {
      provideCompletionItems: async (model, position, context, _token) => {
        if (!this.initialized || model.uri.toString() !== this.openUri) {
          return { suggestions: [] }
        }
        await this.flushSync()
        const list = await this.request<lsp.CompletionParams, lsp.CompletionList | lsp.CompletionItem[] | null>(
          'textDocument/completion',
          {
            textDocument: { uri: model.uri.toString() },
            position: { line: position.lineNumber - 1, character: position.column - 1 },
            context: {
              triggerKind:
                context.triggerKind === this.monaco.languages.CompletionTriggerKind.TriggerCharacter
                  ? lsp.CompletionTriggerKind.TriggerCharacter
                  : lsp.CompletionTriggerKind.Invoked,
              triggerCharacter: context.triggerCharacter,
            },
          },
        )
        if (list == null) {
          return { suggestions: [] }
        }
        const items = Array.isArray(list) ? list : list.items
        const isIncomplete = Array.isArray(list) ? false : Boolean(list.isIncomplete)
        const suggestions = items.map((item) => this.lspCompletionToMonaco(model, position, item))
        return { suggestions, incomplete: isIncomplete }
      },
    })

    this.hoverDisposable = this.monaco.languages.registerHoverProvider('lesma', {
      provideHover: async (model, position, _token) => {
        if (!this.initialized || model.uri.toString() !== this.openUri) {
          return null
        }
        await this.flushSync()
        const h = await this.request<lsp.HoverParams, lsp.Hover | null>('textDocument/hover', {
          textDocument: { uri: model.uri.toString() },
          position: { line: position.lineNumber - 1, character: position.column - 1 },
        })
        if (h == null || h.contents == null) {
          return null
        }
        const contents = h.contents
        if (Array.isArray(contents)) {
          const parts = contents.map((c) => {
            if (typeof c === 'string') {
              return markupToMarkdownString(c)
            }
            if ('language' in c && 'value' in c) {
              return markupToMarkdownString(c)
            }
            return markupToMarkdownString(c as lsp.MarkupContent)
          })
          return { contents: parts }
        }
        if (typeof contents === 'string') {
          return { contents: [{ value: contents }] }
        }
        if ('language' in contents && 'value' in contents) {
          return { contents: [markupToMarkdownString(contents)] }
        }
        return { contents: [markupToMarkdownString(contents as lsp.MarkupContent)] }
      },
    })
  }

  private lspCompletionToMonaco(
    model: ITextModel,
    position: MonacoPosition,
    item: lsp.CompletionItem,
  ): languages.CompletionItem {
    const word = model.getWordUntilPosition(position)
    const defaultRange = new this.monaco.Range(
      position.lineNumber,
      word.startColumn,
      position.lineNumber,
      word.endColumn,
    )
    let range = defaultRange
    let insertText = completionItemLabel(item)
    if (item.textEdit) {
      if ('range' in item.textEdit) {
        range = lspRangeToMonacoRange(this.monaco, item.textEdit.range)
        insertText = item.textEdit.newText
      }
    } else if (item.insertText != null) {
      insertText = item.insertText
    }

    const out: languages.CompletionItem = {
      label: completionItemLabel(item),
      kind: toCompletionItemKind(this.monaco, item.kind),
      insertText,
      range,
      sortText: item.sortText,
      filterText: item.filterText,
      detail: item.detail,
    }

    if (item.insertTextFormat === lsp.InsertTextFormat.Snippet) {
      out.insertTextRules = this.monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet
    }

    if (item.documentation != null) {
      if (typeof item.documentation === 'string' || 'kind' in item.documentation) {
        out.documentation = markupToMarkdownString(item.documentation as lsp.MarkupContent | string)
      }
    }

    return out
  }

  private send(data: unknown): void {
    if (this.ws?.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(data))
    }
  }

  private request<P, R>(method: string, params: P): Promise<R> {
    return new Promise((resolve, reject) => {
      if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
        reject(new Error('LSP WebSocket not connected'))
        return
      }
      const id = ++this.nextId
      this.pending.set(id, { resolve: resolve as (v: unknown) => void, reject })
      this.send({ jsonrpc: '2.0', id, method, params })
    })
  }

  private notify<P>(method: string, params: P): void {
    this.send({ jsonrpc: '2.0', method, params })
  }

  private handleMessage(raw: string): void {
    let msg: {
      id?: number
      result?: unknown
      error?: { message?: string }
      method?: string
      params?: unknown
    }
    try {
      msg = JSON.parse(raw) as typeof msg
    } catch {
      return
    }

    if (msg.id != null && 'result' in msg && msg.result !== undefined) {
      const h = this.pending.get(msg.id)
      if (h) {
        this.pending.delete(msg.id)
        h.resolve(msg.result)
      }
      return
    }

    if (msg.id != null && msg.error) {
      const h = this.pending.get(msg.id)
      if (h) {
        this.pending.delete(msg.id)
        h.reject(new Error(msg.error.message ?? 'LSP error'))
      }
      return
    }

    if (msg.method === 'textDocument/publishDiagnostics' && msg.params) {
      this.applyDiagnostics(msg.params as lsp.PublishDiagnosticsParams)
    }
  }

  private applyDiagnostics(params: lsp.PublishDiagnosticsParams): void {
    const model = this.getActiveModel()
    if (!model || model.uri.toString() !== params.uri) {
      return
    }
    if (params.version != null && params.version !== this.documentVersion) {
      return
    }
    const markers = params.diagnostics.map((d) => ({
      severity: toMarkerSeverity(this.monaco, d.severity),
      startLineNumber: d.range.start.line + 1,
      startColumn: d.range.start.character + 1,
      endLineNumber: d.range.end.line + 1,
      endColumn: d.range.end.character + 1,
      message: d.message,
      source: d.source,
      code: typeof d.code === 'number' || typeof d.code === 'string' ? String(d.code) : undefined,
    }))
    this.monaco.editor.setModelMarkers(model, 'lesma-lsp', markers)
  }

  private async handshake(): Promise<void> {
    try {
      await this.request<lsp.InitializeParams, lsp.InitializeResult>('initialize', {
        processId: null,
        rootUri: 'file:///workspace',
        clientInfo: { name: '@lesma/playground-client', version: '0.1' },
        capabilities: {
          workspace: { workspaceFolders: true },
          textDocument: {
            synchronization: {
              dynamicRegistration: false,
              willSave: false,
              didSave: false,
            },
            completion: {
              dynamicRegistration: false,
              completionItem: {
                snippetSupport: true,
                documentationFormat: [lsp.MarkupKind.Markdown, lsp.MarkupKind.PlainText],
              },
              contextSupport: true,
            },
            hover: {
              dynamicRegistration: false,
              contentFormat: [lsp.MarkupKind.Markdown, lsp.MarkupKind.PlainText],
            },
            publishDiagnostics: {
              relatedInformation: true,
              versionSupport: true,
              tagSupport: {
                valueSet: [lsp.DiagnosticTag.Unnecessary, lsp.DiagnosticTag.Deprecated],
              },
            },
            signatureHelp: { dynamicRegistration: false },
            definition: { dynamicRegistration: false, linkSupport: true },
            declaration: { dynamicRegistration: false, linkSupport: true },
            implementation: { dynamicRegistration: false, linkSupport: true },
            typeDefinition: { dynamicRegistration: false, linkSupport: true },
          },
        },
      })
      this.notify('initialized', {})
      this.initialized = true
      await this.syncOpenIfLesma()
    } catch (e) {
      console.warn('[lesma-lsp] initialize failed', e)
    }
  }

  private clearMarkers(uriStr: string): void {
    const uri = this.monaco.Uri.parse(uriStr)
    const m = this.monaco.editor.getModel(uri)
    if (m) {
      this.monaco.editor.setModelMarkers(m, 'lesma-lsp', [])
    }
  }

  private attachModelListener(model: ITextModel | null): void {
    this.modelSub?.dispose()
    this.modelSub = null
    if (!model) {
      return
    }
    this.modelSub = model.onDidChangeContent(() => {
      this.documentDirty = true
      this.scheduleFlush()
    })
  }

  /** Tab / model switch */
  onActiveModelChanged(): void {
    void this.syncOpenIfLesma()
  }

  private async syncOpenIfLesma(): Promise<void> {
    const model = this.getActiveModel()
    const uri = model?.uri.toString() ?? null
    const want = Boolean(model && uri && isLesmaWorkspaceUri(uri))

    if (this.openUri && (!want || uri !== this.openUri)) {
      if (this.initialized && this.ws?.readyState === WebSocket.OPEN) {
        this.notify('textDocument/didClose', { textDocument: { uri: this.openUri } })
      }
      this.clearMarkers(this.openUri)
      this.openUri = null
      this.documentVersion = 0
    }

    this.attachModelListener(model ?? null)

    if (!want || !model || !uri || !this.initialized || !this.ws || this.ws.readyState !== WebSocket.OPEN) {
      return
    }

    if (uri !== this.openUri) {
      this.documentVersion = 1
      this.openUri = uri
      this.documentDirty = false
      this.notify('textDocument/didOpen', {
        textDocument: {
          uri,
          languageId: 'lesma',
          version: this.documentVersion,
          text: model.getValue(),
        },
      })
    }
  }

  private scheduleFlush(): void {
    if (!this.openUri || !this.initialized) {
      return
    }
    if (this.flushTimer != null) {
      clearTimeout(this.flushTimer)
    }
    this.flushTimer = setTimeout(() => {
      void this.flushNow()
    }, 120)
  }

  private async flushSync(): Promise<void> {
    if (this.flushTimer != null) {
      clearTimeout(this.flushTimer)
      this.flushTimer = null
    }
    await this.flushNow()
  }

  private async flushNow(): Promise<void> {
    const model = this.getActiveModel()
    if (!model || !this.openUri || model.uri.toString() !== this.openUri) {
      return
    }
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN || !this.initialized) {
      return
    }
    if (!this.documentDirty) {
      return
    }
    this.documentDirty = false
    this.documentVersion++
    this.notify<lsp.DidChangeTextDocumentParams>('textDocument/didChange', {
      textDocument: { uri: this.openUri, version: this.documentVersion },
      contentChanges: [{ text: model.getValue() }],
    })
  }

  dispose(): void {
    if (this.flushTimer != null) {
      clearTimeout(this.flushTimer)
      this.flushTimer = null
    }
    this.modelSub?.dispose()
    this.modelSub = null
    this.completionDisposable?.dispose()
    this.completionDisposable = null
    this.hoverDisposable?.dispose()
    this.hoverDisposable = null

    if (this.openUri && this.ws?.readyState === WebSocket.OPEN) {
      this.notify('textDocument/didClose', { textDocument: { uri: this.openUri } })
    }
    this.openUri = null

    for (const [, p] of this.pending) {
      p.reject(new Error('LSP disposed'))
    }
    this.pending.clear()

    this.ws?.close()
    this.ws = null
    this.initialized = false
  }
}
