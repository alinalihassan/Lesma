import type { RunResponse } from './models'

export interface IAPIClient {
  run: (files: Record<string, string>) => Promise<RunResponse>
}
