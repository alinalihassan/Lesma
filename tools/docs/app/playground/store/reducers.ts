import { combineReducers } from 'redux'

import { type EvalEvent } from '@/playground/services/api/models/run'
import config from '@/playground/services/config/config'
import type { MonacoSettings } from '@/playground/services/config/monaco'

import vimReducers from './vim/reducers'
import notificationReducers from './notifications/reducers'

import { initialTerminalState } from './terminal/state'
import { reducers as terminalReducers } from './terminal/reducers'

import { type FilePayload, WorkspaceAction } from '@/playground/store/workspace/actions'
import { getDefaultWorkspaceState } from '@/playground/store/workspace/state'
import { reducers as workspaceReducers } from '@/playground/store/workspace/reducers'

import { type Action, ActionType } from './actions/actions'
import type { MonacoParamsChanges } from './actions/settings'
import type { CursorPositionChangePayload, MarkerChangePayload } from './actions/editor'
import type { LoadingStateChanges } from './actions/ui'
import { readStoredChromeThemeIsDark } from '@/lib/lesma-theme-bridge'

import { mapByAction } from './helpers'

import { type SettingsState, type State, type StatusState, type PanelState, type UIState } from './state'

// TODO: move settings reducers and state to store/settings
const initialSettingsState: SettingsState = {
  autoSave: config.autoSave,
  darkMode: readStoredChromeThemeIsDark() ?? config.darkThemeEnabled,
  autoFormat: true,
  enableVimMode: config.enableVimMode,
  compilerDebugLexer: config.compilerDebugLexer,
  compilerDebugAst: config.compilerDebugAst,
  compilerDebugIr: config.compilerDebugIr,
}

const reducers = {
  status: mapByAction<StatusState>(
    {
      [WorkspaceAction.WORKSPACE_IMPORT]: (_: StatusState) => ({
        loading: false,
        running: false,
        dirty: false,
        lastError: null,
        events: undefined,
      }),
      [WorkspaceAction.SNIPPET_LOAD_FINISH]: (s: StatusState) => ({
        ...s,
        running: false,
        dirty: false,
        lastError: null,
        events: undefined,
      }),
      [WorkspaceAction.SNIPPET_LOAD_START]: (s: StatusState) => ({
        ...s,
        running: false,
        dirty: false,
        lastError: null,
        events: undefined,
      }),
      [WorkspaceAction.REMOVE_FILE]: (
        { markers, ...state }: StatusState,
        { payload: { filename } }: Action<FilePayload>,
      ) => {
        const { [filename]: _, ...newMarkers } = markers ?? {}
        return {
          ...state,
          markers: newMarkers,
        }
      },
      [ActionType.ERROR]: (s: StatusState, a: Action<string>) => ({
        ...s,
        loading: false,
        running: false,
        dirty: true,
        lastError: a.payload,
      }),
      [ActionType.LOADING_STATE_CHANGE]: (s: StatusState, { payload: { loading } }: Action<LoadingStateChanges>) => ({
        ...s,
        loading,
        running: false,
      }),
      [ActionType.EVAL_START]: (s: StatusState, _: Action) => ({
        ...s,
        lastError: null,
        loading: false,
        running: true,
        dirty: true,
        events: [],
      }),
      [ActionType.EVAL_EVENT]: (s: StatusState, a: Action<EvalEvent>) => ({
        ...s,
        lastError: null,
        loading: false,
        dirty: true,
        running: s.running,
        events: s.events ? s.events.concat(a.payload) : [a.payload],
      }),
      [ActionType.EVAL_FINISH]: (s: StatusState, _: Action) => ({
        ...s,
        loading: false,
        running: false,
        dirty: true,
      }),
      [ActionType.MARKER_CHANGE]: (s: StatusState, { payload }: Action<MarkerChangePayload>) => ({
        ...s,
        markers: {
          ...s.markers,
          [payload.fileName]: payload.markers?.length ? payload.markers : null,
        },
      }),
      [ActionType.CURSOR_POSITION_CHANGE]: (s: StatusState, { payload }: Action<CursorPositionChangePayload>) => ({
        ...s,
        cursorPosition: payload.position,
      }),
    },
    { loading: false },
  ),
  settings: mapByAction<SettingsState>(
    {
      [ActionType.TOGGLE_THEME]: (s: SettingsState, a: Action) => {
        s.darkMode = !s.darkMode
        config.darkThemeEnabled = s.darkMode
        return s
      },
      [ActionType.SETTINGS_CHANGE]: (s: SettingsState, { payload }: Action<Partial<SettingsState>>) => ({
        ...s,
        ...payload,
      }),
    },
    initialSettingsState,
  ),
  monaco: mapByAction<MonacoSettings>(
    {
      [ActionType.MONACO_SETTINGS_CHANGE]: (s: MonacoSettings, { payload }: Action<MonacoParamsChanges>) => ({
        ...s,
        ...payload,
      }),
    },
    config.monacoSettings,
  ),
  panel: mapByAction<PanelState>(
    {
      [ActionType.PANEL_STATE_CHANGE]: (s: PanelState, { payload }: Action<PanelState>) => ({
        ...s,
        ...payload,
      }),
    },
    config.panelLayout,
  ),
  ui: mapByAction<UIState>(
    {
      [ActionType.LOADING_STATE_CHANGE]: (s: UIState, { payload: { loading } }: Action<LoadingStateChanges>) => {
        if (!s) {
          return { loading }
        }

        return {
          ...s,
          loading,
        }
      },
      [ActionType.UI_STATE_CHANGE]: (s: UIState, { payload }: Action<Partial<UIState>>) => {
        if (!s) {
          return payload as UIState
        }

        return { ...s, ...payload }
      },
    },
    {},
  ),
  vim: vimReducers,
  notifications: notificationReducers,
  terminal: terminalReducers,
  workspace: workspaceReducers,
}

export const getInitialState = (): State => ({
  status: {
    loading: false,
    running: false,
    dirty: false,
    lastError: null,
    events: undefined,
  },
  settings: initialSettingsState,
  monaco: config.monacoSettings,
  panel: config.panelLayout,
  notifications: {},
  vim: null,
  terminal: initialTerminalState,
  workspace: getDefaultWorkspaceState(),
})

export const rootReducer = combineReducers(reducers)
