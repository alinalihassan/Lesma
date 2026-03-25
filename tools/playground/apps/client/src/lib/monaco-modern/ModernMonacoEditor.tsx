import React, { useCallback, useEffect, useRef, useState } from 'react'

import environment from '~/environment'
import {
  type DocumentState,
  type EditorPreferences,
  EventType,
  type MonacoEditorProps,
  Syntax,
} from '~/lib/editor'

import type { editor } from 'modern-monaco/editor-core'

import { applyMonacoColorScheme, ensureMonaco, type MonacoApi } from './init-monaco'
import { LesmaMonacoLspBridge } from './lesma-lsp-bridge'
import { MonacoEditorRemote } from './remote'
import { workspaceFileUri } from './workspace-uri'

import classes from './ModernMonacoEditor.module.css'

function languageIdForPath(path: string): string {
  if (path.endsWith('.les')) {
    return 'lesma'
  }
  if (path.endsWith('.json')) {
    return 'json'
  }
  return 'plaintext'
}

function syntaxForPath(path: string): Syntax {
  if (path.endsWith('.les')) {
    return Syntax.Lesma
  }
  if (path.endsWith('.json')) {
    return Syntax.JSON
  }
  return Syntax.PlainText
}

function preferencesWithDefaults(p?: EditorPreferences): EditorPreferences {
  const base: EditorPreferences = {
    colorScheme: 'light',
    inputMode: 'default',
    tabSize: 4,
    fontSize: 14,
    fontFamily: 'Menlo, Monaco, "Courier New", monospace',
    showLineNumbers: true,
  }
  return { ...base, ...p }
}

/**
 * Playground code editor backed by [modern-monaco](https://github.com/esm-dev/modern-monaco) (Monaco + Shiki).
 */
export const ModernMonacoEditor: React.FC<MonacoEditorProps> = (props) => {
  const hostRef = useRef<HTMLDivElement>(null)
  /** Must gate document/LSP effects: they run before `ensureMonaco()` resolves on first paint. */
  const [monacoReady, setMonacoReady] = useState(false)
  const monacoRef = useRef<MonacoApi | null>(null)
  const editorRef = useRef<editor.IStandaloneCodeEditor | null>(null)
  const remoteRef = useRef<MonacoEditorRemote | null>(null)
  const lspRef = useRef<LesmaMonacoLspBridge | null>(null)
  const openPathRef = useRef<string | null>(null)
  const propsRef = useRef(props)
  propsRef.current = props

  const disposeLsp = useCallback(() => {
    lspRef.current?.dispose()
    lspRef.current = null
  }, [])

  useEffect(() => {
    const host = hostRef.current
    if (!host) {
      return
    }

    let cancelled = false

    void ensureMonaco().then((monaco) => {
      if (cancelled) {
        return
      }
      monacoRef.current = monaco

      const remote = new MonacoEditorRemote(monaco, () => editorRef.current)
      remoteRef.current = remote

      const ed = monaco.editor.create(host, {
        automaticLayout: true,
        model: null,
        minimap: { enabled: true },
        scrollBeyondLastLine: false,
        wordWrap: 'on',
      })
      editorRef.current = ed
      if (!cancelled) {
        setMonacoReady(true)
        requestAnimationFrame(() => {
          ed.layout()
        })
      }

      propsRef.current.onMount?.(remote)

      const prefs = preferencesWithDefaults(propsRef.current.preferences)
      applyMonacoColorScheme(monaco, prefs.colorScheme)
      ed.updateOptions({
        readOnly: Boolean(propsRef.current.readonly),
        tabSize: prefs.tabSize,
        insertSpaces: true,
        fontFamily: prefs.fontFamily,
        fontSize: prefs.fontSize,
        fontLigatures: prefs.fontLigatures ? 'on' : 'off',
        lineNumbers:
          prefs.inputMode === 'vim' && prefs.vimUseRelativeLineNumbers ? 'relative' : 'on',
      })

      ed.onDidChangeCursorPosition(() => {
        const p = propsRef.current
        const pos = ed.getPosition()
        if (!pos) {
          return
        }
        p.onEvent?.({
          type: EventType.CursorPositionChanged,
          position: { line: pos.lineNumber, column: pos.column },
        })
      })

      ed.onDidChangeModelContent(() => {
        const p = propsRef.current
        const model = ed.getModel()
        const path = openPathRef.current
        if (!model || !path) {
          return
        }
        const doc: DocumentState = {
          path,
          language: syntaxForPath(path),
          text: model.getValue(),
        }
        p.onChange?.(doc)
      })

      ed.onDidChangeModel(() => {
        lspRef.current?.onActiveModelChanged()
      })
    })

    return () => {
      cancelled = true
      setMonacoReady(false)
      disposeLsp()
      propsRef.current.onUnmount?.()
      editorRef.current?.dispose()
      editorRef.current = null
      monacoRef.current = null
      remoteRef.current = null
      openPathRef.current = null
    }
  }, [disposeLsp])

  /* Workspace reset: drop virtual files + LSP (content reloads on next open). */
  useEffect(() => {
    const monaco = monacoRef.current
    if (!monaco) {
      return
    }
    disposeLsp()
    for (const m of monaco.editor.getModels()) {
      if (m.uri.toString().startsWith('file:///workspace/')) {
        m.dispose()
      }
    }
    openPathRef.current = null
    editorRef.current?.setModel(null)
  }, [props.workspaceKey, disposeLsp])

  /* Theme / chrome */
  useEffect(() => {
    const monaco = monacoRef.current
    const ed = editorRef.current
    if (!monaco || !ed || !monacoReady) {
      return
    }
    const prefs = preferencesWithDefaults(props.preferences)
    applyMonacoColorScheme(monaco, prefs.colorScheme)
    ed.updateOptions({
      readOnly: Boolean(props.readonly),
      tabSize: prefs.tabSize,
      insertSpaces: true,
      fontFamily: prefs.fontFamily,
      fontSize: prefs.fontSize,
      fontLigatures: prefs.fontLigatures ? 'on' : 'off',
      lineNumbers:
        prefs.inputMode === 'vim' && prefs.vimUseRelativeLineNumbers ? 'relative' : 'on',
    })
    ed.layout()
  }, [props.preferences, props.readonly, monacoReady])

  /**
   * Open document + LSP: drive only from path / workspace generation.
   * Do not depend on `content` here — Redux updates content every keystroke and would reconnect LSP each time.
   */
  useEffect(() => {
    const monaco = monacoRef.current
    const ed = editorRef.current
    if (!monaco || !ed || !monacoReady) {
      return
    }

    const doc = props.value
    if (!doc) {
      disposeLsp()
      ed.setModel(null)
      openPathRef.current = null
      return
    }

    const { path, content } = doc
    const uri = monaco.Uri.parse(workspaceFileUri(path))
    const lang = languageIdForPath(path)

    const prevPath = openPathRef.current
    const pathChanged = prevPath !== path

    let model = monaco.editor.getModel(uri)
    if (!model) {
      model = monaco.editor.createModel(content, lang, uri)
    } else if (pathChanged && model.getValue() !== content) {
      model.setValue(content)
    }

    ed.setModel(model)
    openPathRef.current = path

    disposeLsp()
    if (path.endsWith('.les')) {
      const bridge = new LesmaMonacoLspBridge(monaco, environment.lspWebSocketUrl, () => ed.getModel())
      lspRef.current = bridge
      bridge.start()
      bridge.onActiveModelChanged()
    }

    ed.focus()
    requestAnimationFrame(() => {
      ed.layout()
    })
  }, [props.value?.path, props.workspaceKey, disposeLsp, monacoReady])

  const prefs = preferencesWithDefaults(props.preferences)

  return (
    <div
      ref={hostRef}
      className={classes.gpgMonacoHost}
      data-theme={prefs.colorScheme}
      data-font-ligatures={prefs.fontLigatures ? 'true' : 'false'}
    />
  )
}
