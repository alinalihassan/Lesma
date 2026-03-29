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
  /** Plain text output avoids xterm layout/addon issues in embedded docs and matches Settings guidance. */
  disableTerminalEmulation: true,
}
