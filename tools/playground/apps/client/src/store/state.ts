import type { Diagnostic } from 'vscode-languageserver-protocol'
import type { EvalEvent } from '~/services/api/models/run'
import type { MonacoSettings } from '~/services/config/monaco'
import type { LayoutType } from '~/styles/layout'

import type { VimState } from './vim/state'
import { type NotificationsState } from './notifications/state'
import type { TerminalState } from './terminal/state'
import type { WorkspaceState } from './workspace/state'

export type InspectorOutputTab = 'terminal' | 'problems'

export interface PendingEditorReveal {
  path: string
  /** 1-based line (Monaco) */
  line: number
  /** 1-based column (Monaco) */
  column: number
}

export interface UIState {
  loading?: boolean
  /** Bottom panel: terminal vs Problems list */
  inspectorTab?: InspectorOutputTab
  /** After switching file, editor scrolls to this location */
  pendingEditorReveal?: PendingEditorReveal | null
}

export interface Position {
  line: number
  column: number
}

export interface StatusState {
  loading: boolean
  running?: boolean
  dirty?: boolean
  lastError?: string | null
  events?: EvalEvent[]
  markers?: Record<string, Diagnostic[] | null>
  cursorPosition?: Position
}

export interface SettingsState {
  darkMode: boolean
  useSystemTheme: boolean
  autoSave: boolean
  autoFormat: boolean
  enableVimMode: boolean
  /** Pass `-d lexer` to `lesma run` when running from the playground. */
  compilerDebugLexer: boolean
  /** Pass `-d ast` to `lesma run`. */
  compilerDebugAst: boolean
  /** Pass `-d ir` to `lesma run`. */
  compilerDebugIr: boolean
}

export interface PanelState {
  height?: number
  widthPercent?: number
  collapsed?: boolean
  layout?: LayoutType
}

export interface State {
  status?: StatusState
  settings: SettingsState
  monaco: MonacoSettings
  panel: PanelState
  ui?: UIState
  vim?: VimState | null
  workspace: WorkspaceState
  notifications: NotificationsState
  terminal: TerminalState
}
