import { connect } from 'react-redux';
import {editor} from "monaco-editor";
import { EvalEvent } from '@site/src/services/api';
import { MonacoSettings, RuntimeType } from '@site/src/services/config';
import {LayoutType} from '@site/src/styles/layout';

export interface UIState {
  shareCreated?: boolean
  snippetId?: string | null
}

export interface EditorState {
  fileName: string,
  code: string
}

export interface StatusState {
  loading: boolean,
  lastError?: string | null,
  events?: EvalEvent[],
  markers?: editor.IMarkerData[]
}

export interface SettingsState {
  darkMode: boolean
  useSystemTheme: boolean
  autoFormat: boolean,
  runtime: RuntimeType
}

export interface PanelState {
  height ?: string|number
  width ?: string|number
  collapsed ?: boolean
  layout?: LayoutType
}

export interface State {
  editor: EditorState
  status?: StatusState,
  settings: SettingsState
  monaco: MonacoSettings
  panel: PanelState
  ui?: UIState
}

export function Connect(fn: (state: State) => any) {
  return function (constructor: Function) {
    return connect(fn)(constructor as any) as any;
  }
}