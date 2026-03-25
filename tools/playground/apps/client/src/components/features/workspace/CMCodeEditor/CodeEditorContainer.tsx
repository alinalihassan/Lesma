import React, { useCallback, useEffect, useMemo, useRef } from 'react'
import { useDispatch, useSelector } from 'react-redux'
import type { AnyAction } from 'redux'
import type { Diagnostic } from 'vscode-languageserver-protocol'

import {
  CommandType,
  defaultEditorPreferences,
  type Document,
  type EditorCommand,
  type EditorEvent,
  type EditorPreferences,
  type EditorRemote,
  EventType,
} from '~/lib/editor'
import { ModernMonacoEditor } from '~/lib/monaco-modern/ModernMonacoEditor'
import type { State } from '~/store/state'
import { VimMode, VimSubMode } from '~/store/vim/state'
import { newVimDisposeAction, newVimModeChangeAction } from '~/store/vim/actions'
import { dispatchUpdateFile } from '~/store/workspace'
import { getDefaultFontFamily, getFontFamily } from '~/services/fonts'
import {
  Dispatcher,
  newMarkerAction,
  newUIStateChangeAction,
  newCursorPositionChangeDispatcher,
  newMonacoParamsChangeDispatcher,
  runFileDispatcher,
} from '~/store'
import { useDebouncer } from '~/hooks/debounce'

const preferencesWithDefaults = (src: Partial<EditorPreferences>): EditorPreferences =>
  Object.assign(Object.create(defaultEditorPreferences), src)

const mapEventToAction = (e: EditorEvent): AnyAction | Dispatcher | undefined => {
  switch (e.type) {
    case EventType.VimModeChanged: {
      const subMode =
        e.mode === 'visual' && e.subMode === 'linewise'
          ? VimSubMode.Linewise
          : e.mode === 'visual' && e.subMode === 'blockwise'
            ? VimSubMode.Blockwise
            : undefined
      return newVimModeChangeAction({
        mode: e.mode as VimMode,
        subMode,
      })
    }
    case EventType.InputModeChanged:
      if (e.mode === 'default' && e.prevMode === 'vim') {
        return newVimDisposeAction()
      }
      if (e.mode === 'vim' && e.prevMode !== 'vim') {
        return newVimModeChangeAction({ mode: VimMode.Normal })
      }
      break
    case EventType.CursorPositionChanged: {
      return newCursorPositionChangeDispatcher(e.position)
    }
    default:
      break
  }
}

const mapCommandToAction = (e: EditorCommand, _rem: EditorRemote): AnyAction | Dispatcher | undefined => {
  switch (e.type) {
    case CommandType.EditorZoom:
      return newMonacoParamsChangeDispatcher({
        fontSize: e.newSize,
      })
    case CommandType.Run:
      return runFileDispatcher
    default:
  }
}

export interface CodeEditorContainerProps {
  onMount: (remote: EditorRemote) => void
  onUnmount: () => void
}

/**
 * Connects the playground editor (modern-monaco) to Redux and run actions.
 */
export const CodeEditorContainer: React.FC<CodeEditorContainerProps> = ({ onMount, onUnmount }) => {
  const dispatch = useDispatch()
  const saveDebouncer = useDebouncer(150)
  const editorRemoteRef = useRef<EditorRemote | null>(null)

  const monaco = useSelector((state: State) => state.monaco)
  const settings = useSelector((state: State) => state.settings)
  const workspace = useSelector((state: State) => state.workspace)
  const pendingReveal = useSelector((state: State) => state.ui?.pendingEditorReveal)
  const isReadOnly = useSelector(({ status, workspace: ws }: State) =>
    Boolean(status?.loading || status?.running || ws.snippet?.loading),
  )

  const preferences: EditorPreferences = useMemo(
    () =>
      preferencesWithDefaults({
        colorScheme: settings.darkMode ? 'dark' : 'light',
        inputMode: settings.enableVimMode ? 'vim' : 'default',
        fontFamily: monaco.fontFamily ? getFontFamily(monaco.fontFamily) : getDefaultFontFamily(),
        fontSize: monaco.fontSize ?? defaultEditorPreferences.fontSize,
        tabSize: monaco.tabSize,
        fontLigatures: monaco.fontLigatures,
        vimUseSystemClipboard: monaco.vimUseSystemClipboard,
        vimUseRelativeLineNumbers: monaco.vimUseRelativeLineNumbers,
      }),
    [settings, monaco],
  )

  const doc: Document | undefined = useMemo(() => {
    const { selectedFile, files } = workspace

    if (selectedFile && files) {
      return {
        path: selectedFile,
        content: files[selectedFile],
      }
    }
  }, [workspace])

  const onDiagnostics = useCallback(
    (workspacePath: string, diagnostics: Diagnostic[]) => {
      dispatch(newMarkerAction(workspacePath, diagnostics))
    },
    [dispatch],
  )

  const handleEditorMount = useCallback(
    (remote: EditorRemote) => {
      editorRemoteRef.current = remote
      onMount(remote)
    },
    [onMount],
  )

  useEffect(() => {
    return () => {
      editorRemoteRef.current = null
    }
  }, [])

  useEffect(() => {
    if (!pendingReveal) {
      return
    }
    if (pendingReveal.path !== workspace.selectedFile) {
      return
    }
    const remote = editorRemoteRef.current
    if (!remote) {
      return
    }
    /* Defer past Monaco model swap when switching tabs */
    const id = window.setTimeout(() => {
      remote.revealPosition(pendingReveal.path, pendingReveal.line, pendingReveal.column)
      dispatch(newUIStateChangeAction({ pendingEditorReveal: null }))
    }, 32)
    return () => clearTimeout(id)
  }, [pendingReveal, workspace.selectedFile, dispatch])

  return (
    <div
      style={{
        display: 'flex',
        flex: '1 1 auto',
        flexDirection: 'column',
        minHeight: 0,
        minWidth: 0,
        width: '100%',
        height: '100%',
      }}
    >
      <ModernMonacoEditor
        workspaceKey={workspace.generation}
        value={doc}
        preferences={preferences}
        readonly={isReadOnly}
        onMount={handleEditorMount}
        onUnmount={onUnmount}
        onChange={({ path, text }) => {
          saveDebouncer(() => {
            dispatch(dispatchUpdateFile(path, text))
          })
        }}
        onEvent={(e) => {
          const action = mapEventToAction(e)
          if (action) {
            dispatch(action)
          }
        }}
        onCommand={(cmd, rem) => {
          const action = mapCommandToAction(cmd, rem)
          if (action) {
            dispatch(action)
          }
        }}
        onDiagnostics={onDiagnostics}
      />
    </div>
  )
}

export default CodeEditorContainer
