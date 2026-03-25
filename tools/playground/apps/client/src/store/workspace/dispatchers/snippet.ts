import type { DispatchFn, StateProvider } from '~/store/helpers'
import { getSnippetFromSource, type SnippetSource } from '~/services/examples'
import { newRemoveNotificationAction, NotificationIDs } from '~/store/notifications'
import { type SnippetLoadPayload, WorkspaceAction } from '../actions'
import { loadWorkspaceState } from '../config'
import { getDefaultWorkspaceState } from '../state'

let snippetFromSourceSeq = 0
let snippetFromSourceAbort: AbortController | null = null

/**
 * Dispatch snippet load from a predefined source (static example files).
 */
export const dispatchLoadSnippetFromSource = (source: SnippetSource) => async (dispatch: DispatchFn) => {
  const seq = ++snippetFromSourceSeq
  snippetFromSourceAbort?.abort()
  snippetFromSourceAbort = new AbortController()
  const { signal } = snippetFromSourceAbort

  dispatch(newRemoveNotificationAction(NotificationIDs.GoModMissing))
  dispatch({
    type: WorkspaceAction.SNIPPET_LOAD_START,
    payload: source.basePath,
  })

  try {
    const files = await getSnippetFromSource(source, signal)
    if (seq !== snippetFromSourceSeq) {
      return
    }
    dispatch<SnippetLoadPayload>({
      type: WorkspaceAction.SNIPPET_LOAD_FINISH,
      payload: {
        id: source.basePath,
        error: null,
        files,
      },
    })
  } catch (err: any) {
    if (seq !== snippetFromSourceSeq) {
      return
    }
    const message = err instanceof Error ? err.message : String(err)
    if (err?.name === 'AbortError') {
      return
    }
    dispatch<SnippetLoadPayload>({
      type: WorkspaceAction.SNIPPET_LOAD_FINISH,
      payload: {
        id: source.basePath,
        error: message,
      },
    })
  }
}

/**
 * Initial workspace: autosaved state (if enabled) or default example.
 */
export const dispatchInitWorkspace = () => async (dispatch: DispatchFn, getState: StateProvider) => {
  const {
    settings: { autoSave },
    workspace: { snippet },
  } = getState()

  const shouldAutosave = autoSave && !snippet?.id
  dispatch({
    type: WorkspaceAction.WORKSPACE_IMPORT,
    payload: shouldAutosave ? loadWorkspaceState() : getDefaultWorkspaceState(),
  })
}
