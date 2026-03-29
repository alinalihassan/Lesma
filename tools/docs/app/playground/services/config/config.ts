import type { PanelState } from '@/playground/store/state'
import { defaultPanelProps } from '@/playground/styles/layout'

import { type MonacoSettings, defaultMonacoSettings } from './monaco'

const DARK_THEME_KEY = 'ui.darkTheme.enabled'
const AUTOSAVE_ENABLED = 'ui.autosave.enabled'
const ENABLE_VIM_MODE_KEY = 'ms.monaco.vimModeEnabled'
const AUTOFORMAT_KEY = 'go.build.autoFormat'
const MONACO_SETTINGS = 'ms.monaco.settings'
const PANEL_SETTINGS = 'ui.layout.panel'
const COMPILER_DEBUG_LEXER_KEY = 'lesma.compiler.debugLexer'
const COMPILER_DEBUG_AST_KEY = 'lesma.compiler.debugAst'
const COMPILER_DEBUG_IR_KEY = 'lesma.compiler.debugIr'

// TODO: move key operations to store.

// Do not call Fluent `loadTheme` here: it injects global CSS (load-themed-styles) that persists
// after React Router navigates away from `/playground` and breaks the docs chrome. Playground
// Fluent trees use `ThemeProvider` + `ConnectedThemeProvider` only.

const Config = {
  _cache: new Map<string, any>(),

  get darkThemeEnabled(): boolean {
    return this.getBoolean(DARK_THEME_KEY, false)
  },

  set darkThemeEnabled(enable: boolean) {
    this.setBoolean(DARK_THEME_KEY, enable)
  },

  get autoSave() {
    return this.getBoolean(AUTOSAVE_ENABLED, false)
  },

  set autoSave(val: boolean) {
    this.setBoolean(AUTOSAVE_ENABLED, val)
  },

  get enableVimMode() {
    return this.getBoolean(ENABLE_VIM_MODE_KEY, false)
  },

  set enableVimMode(val: boolean) {
    this.setBoolean(ENABLE_VIM_MODE_KEY, val)
  },

  get autoFormat(): boolean {
    return this.getBoolean(AUTOFORMAT_KEY, true)
  },

  set autoFormat(v: boolean) {
    this.setBoolean(AUTOFORMAT_KEY, v)
  },

  get monacoSettings(): MonacoSettings {
    return this.getObject<MonacoSettings>(MONACO_SETTINGS, defaultMonacoSettings)
  },

  set monacoSettings(m: MonacoSettings) {
    this.setObject(MONACO_SETTINGS, m)
  },

  get panelLayout(): PanelState {
    return this.getObject<PanelState>(PANEL_SETTINGS, {
      ...defaultPanelProps,
    })
  },

  set panelLayout(v: PanelState) {
    this.setObject(PANEL_SETTINGS, v)
  },

  get compilerDebugLexer(): boolean {
    return this.getBoolean(COMPILER_DEBUG_LEXER_KEY, false)
  },

  set compilerDebugLexer(v: boolean) {
    this.setBoolean(COMPILER_DEBUG_LEXER_KEY, v)
  },

  get compilerDebugAst(): boolean {
    return this.getBoolean(COMPILER_DEBUG_AST_KEY, false)
  },

  set compilerDebugAst(v: boolean) {
    this.setBoolean(COMPILER_DEBUG_AST_KEY, v)
  },

  get compilerDebugIr(): boolean {
    return this.getBoolean(COMPILER_DEBUG_IR_KEY, false)
  },

  set compilerDebugIr(v: boolean) {
    this.setBoolean(COMPILER_DEBUG_IR_KEY, v)
  },

  getString<T = string>(key: string, defaultVal: T) {
    if (this._cache.has(key)) {
      return this._cache.get(key)
    }

    const val = localStorage.getItem(key)
    return (val ?? defaultVal) as T
  },

  setString(key: string, val: string) {
    this._cache.set(key, val)
    localStorage.setItem(key, val)
  },

  getBoolean(key: string, defVal: boolean): boolean {
    if (this._cache.has(key)) {
      return this._cache.get(key)
    }

    const val = localStorage.getItem(key)
    if (!val) {
      return defVal
    }

    return val === 'true'
  },

  setBoolean(key: string, val: boolean) {
    this._cache.set(key, val)
    localStorage.setItem(key, val.toString())
  },

  getObject<T>(key: string, fallback: T): T {
    if (this._cache.has(key)) {
      return this._cache.get(key) as T
    }

    const val = localStorage.getItem(key)
    if (!val) {
      this._cache.set(key, fallback)
      return fallback
    }

    try {
      const obj = JSON.parse(val) as T
      const result = fallback ? { ...fallback, ...obj } : obj
      this._cache.set(key, result)
      return result
    } catch (err) {
      console.warn(`failed to read settings key ${key}`, err)
      this._cache.set(key, fallback)
      return fallback
    }
  },

  setObject<T>(key: string, val: T) {
    this._cache.set(key, val)
    try {
      localStorage.setItem(key, JSON.stringify(val))
    } catch (err) {
      console.error(`Failed to save ${key} property to localStorage:`, err)
    }
  },

  delete(key: string) {
    localStorage.removeItem(key)
  },

  sync() {
    /* intentionally empty — see note above loadTheme */
  },

  forceRefreshPage() {
    // document.location.reload(true);
  },
}

export default Config
export type IConfig = typeof Config
