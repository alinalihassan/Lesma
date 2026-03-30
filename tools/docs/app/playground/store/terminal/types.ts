export interface TerminalSettings {
  fontSize: number
  disableTerminalEmulation?: boolean
}

export const defaultTerminalSettings: TerminalSettings = {
  fontSize: 14,
  /** Plain panel (no xterm): SGR colors still render via Anser; full terminal uses xterm when this is off. */
  disableTerminalEmulation: true,
}
