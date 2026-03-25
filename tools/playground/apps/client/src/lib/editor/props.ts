import type { Diagnostic } from 'vscode-languageserver-protocol'

import type { ColorScheme, DocumentState, EditorRemote, InputMode } from './types/common'
import type { EditorCommand, EditorEvent } from './types/events'

export interface Document {
  path: string
  content: string
}

export const defaultEditorPreferences: Readonly<EditorPreferences> = {
  colorScheme: 'light',
  inputMode: 'default',
  tabSize: 4,
  fontSize: 14,
  fontFamily: 'Menlo, Monaco, "Courier New", monospace',
  showLineNumbers: true,
}

export interface EditorPreferences {
  colorScheme: ColorScheme
  fontFamily: string
  fontSize: number
  tabSize: number
  fontLigatures?: boolean
  inputMode: InputMode
  vimUseSystemClipboard?: boolean
  vimUseRelativeLineNumbers?: boolean
  showLineNumbers: boolean
}

export interface MonacoEditorProps {
  preferences?: EditorPreferences
  readonly?: boolean
  workspaceKey?: string | number | null
  value?: Document
  onChange?: (e: DocumentState) => void
  onMount?: (r: EditorRemote) => void
  onUnmount?: () => void
  onEvent?: (e: EditorEvent) => void
  onCommand?: (cmd: EditorCommand, rem: EditorRemote) => void
  /** LSP diagnostics for the active document (drives status bar problem counts). */
  onDiagnostics?: (workspacePath: string, diagnostics: Diagnostic[]) => void
}

export type { DocumentState, EditorRemote } from './types/common'
