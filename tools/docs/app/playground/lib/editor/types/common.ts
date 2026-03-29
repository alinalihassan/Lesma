/**
 * Input layout type (Vim/Emacs are not wired for Monaco yet; values are accepted for settings UX).
 */
export type InputMode = 'default' | 'vim' | 'emacs'

export type ColorScheme = 'dark' | 'light'

export type Callback<T> = (arg: T) => void

export enum Syntax {
  PlainText,
  JSON,
  Lesma,
}

export interface DocumentState {
  path: string
  language: Syntax
  /** Full document text (Monaco path); callers may use `.toString()` for compatibility. */
  text: string
}

export interface Position {
  /**
   * Line number. Starts at 1.
   */
  readonly line: number
  readonly column: number
}

export interface Range {
  readonly start: Position
  readonly end: Position
}

/**
 * Offscreen control surface for the playground editor (format, focus, buffer eviction).
 */
export interface EditorRemote {
  formatDocument: (path: string) => void
  invalidateDocument: (path: string) => void
  forgetDocument: (path: string) => void
  focus: () => void
  /** Scroll the open editor to a 1-based line/column (Monaco coordinates). */
  revealPosition: (path: string, line: number, column: number) => void
  dispose: () => void
}
