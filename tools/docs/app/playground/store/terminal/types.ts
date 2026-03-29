export enum RenderingBackend {
  DOM = 'dom',
  WebGL = 'webgl',
  Canvas = 'canvas',
}

export interface TerminalSettings {
  fontSize: number
  renderingBackend: RenderingBackend
  disableTerminalEmulation?: boolean
}

export const defaultTerminalSettings: TerminalSettings = {
  renderingBackend: RenderingBackend.Canvas,
  fontSize: 14,
  /** Plain panel (no xterm): SGR colors still render via Anser; full terminal uses xterm when this is off. */
  disableTerminalEmulation: true,
}
