export enum EvalEventKind {
  Stdout = 'stdout',
  Stderr = 'stderr',
}

export interface EvalEvent {
  Message: string
  Kind: EvalEventKind
  Delay: number
}

export interface RunResponse {
  events: EvalEvent[]
}

export interface FilesPayload {
  files: Record<string, string>
}
