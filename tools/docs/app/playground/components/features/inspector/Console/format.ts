import type { EvalEvent } from '@/playground/services/api/models/run'

/** Pass run output through to xterm (ANSI from Lesma stderr is preserved). */
export const formatEvalEvent = (ev: EvalEvent) => ev.Message ?? ''
