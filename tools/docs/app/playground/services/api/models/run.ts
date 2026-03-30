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

/**
 * Coerce JSON run events into {@link EvalEvent} (handles camelCase or partial objects).
 */
export function normalizeEvalEvent(raw: unknown): EvalEvent {
  if (raw === null || raw === undefined || typeof raw !== 'object') {
    return {
      Message: raw === undefined || raw === null ? '' : String(raw),
      Kind: EvalEventKind.Stdout,
      Delay: 0,
    }
  }

  const o = raw as Record<string, unknown>
  const msg = o.Message ?? o.message ?? ''
  const kindRaw = o.Kind ?? o.kind ?? EvalEventKind.Stdout
  const delay = Number(o.Delay ?? o.delay ?? 0)

  const kind =
    kindRaw === EvalEventKind.Stderr || kindRaw === 'stderr'
      ? EvalEventKind.Stderr
      : EvalEventKind.Stdout

  return {
    Message: typeof msg === 'string' ? msg : String(msg),
    Kind: kind,
    Delay: Number.isFinite(delay) ? delay : 0,
  }
}

export interface FilesPayload {
  files: Record<string, string>
}
