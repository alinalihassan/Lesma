import type { RunResponse } from './models/run'

export type RunRequestOptions = {
  /** Lesma `-d` flags: lexer, ast, ir (matches CLI). */
  debug?: string[]
}

export interface IAPIClient {
  run: (files: Record<string, string>, options?: RunRequestOptions) => Promise<RunResponse>
}
