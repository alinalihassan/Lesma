import React, { useMemo } from 'react'
import { useDispatch, useSelector } from 'react-redux'
import type { AnyAction } from 'redux'
import {
  defaultEditorPreferences,
  Editor,
  type EditorPreferences,
  type Document,
  EventType,
  type EditorEvent,
  type EditorCommand,
  CommandType,
  type EditorRemote,
} from '~/lib/cm-react'
import type { State } from '~/store/state'
import { VimMode, VimSubMode } from '~/store/vim/state'
import { newVimDisposeAction, newVimModeChangeAction } from '~/store/vim/actions'
import { dispatchShareSnippet, dispatchUpdateFile } from '~/store/workspace'
import { getDefaultFontFamily, getFontFamily } from '~/services/fonts'
import {
  Dispatcher,
  newCursorPositionChangeDispatcher,
  newMonacoParamsChangeDispatcher,
  runFileDispatcher,
} from '~/store'
import { useDebouncer } from '~/hooks/debounce'
import { useLesmaLspExtension } from '~/hooks/use-lesma-lsp'

const preferencesWithDefaults = (src: Partial<EditorPreferences>): EditorPreferences =>
  Object.assign(Object.create(defaultEditorPreferences), src)

const mapEventToAction = (e: EditorEvent): AnyAction | Dispatcher | undefined => {
  switch (e.type) {
    case EventType.VimModeChanged:
      return newVimModeChangeAction({
        mode: e.mode as VimMode,
        subMode: e.subMode as VimSubMode,
      })
    case EventType.InputModeChanged:
      switch ('vim') {
        case e.prevMode:
          return newVimDisposeAction()
        case e.mode:
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

const mapCommandToAction = (e: EditorCommand, rem: EditorRemote): AnyAction | Dispatcher | undefined => {
  switch (e.type) {
    case CommandType.EditorZoom:
      return newMonacoParamsChangeDispatcher({
        fontSize: e.newSize,
      })
    case CommandType.Run:
      return runFileDispatcher
    case CommandType.Share:
      return dispatchShareSnippet()
    default:
  }
}

export interface CodeEditorContainerProps {
  onMount: (remote: EditorRemote) => void
  onUnmount: () => void
}

/**
 * Connects CodeMirror code editor to the application store and business logic.
 */
export const CodeEditorContainer: React.FC<CodeEditorContainerProps> = ({ onMount, onUnmount }) => {
  const dispatch = useDispatch()
  const saveDebouncer = useDebouncer(150)

  const monaco = useSelector((state: State) => state.monaco)
  const settings = useSelector((state: State) => state.settings)
  const workspace = useSelector((state: State) => state.workspace)
  const isReadOnly = useSelector(
    ({ status, workspace }: State) =>
      Boolean(status?.loading || status?.running || workspace.snippet?.loading),
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

  const lspExtensions = useLesmaLspExtension(workspace.selectedFile)

  return (
    <Editor
      workspaceKey={workspace.generation}
      value={doc}
      preferences={preferences}
      readonly={isReadOnly}
      lspExtensions={lspExtensions}
      onMount={onMount}
      onUnmount={onUnmount}
      onChange={({ path, text }) => {
        saveDebouncer(() => {
          dispatch(dispatchUpdateFile(path, text.toString()))
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
    />
  )
}

export default CodeEditorContainer
