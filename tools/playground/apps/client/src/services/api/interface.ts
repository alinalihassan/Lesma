import type { RunResponse } from './models/run'

export interface IAPIClient {
  run: (files: Record<string, string>) => Promise<RunResponse>
}
