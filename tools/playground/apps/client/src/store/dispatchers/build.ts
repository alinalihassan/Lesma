import client from '~/services/api/singleton'
import type { DispatchFn, StateProvider } from '~/store/helpers'
import { newProgramFinishAction, newProgramStartAction, newProgramWriteAction } from '~/store/actions/build'
import { newErrorAction } from '~/store/actions/ui'

import type { Dispatcher } from './utils'

/**
 * Runs the current workspace on the playground API (`POST /v2/run`).
 */
export const runFileDispatcher: Dispatcher = async (dispatch: DispatchFn, getState: StateProvider) => {
  const { workspace, settings } = getState()
  const { files } = workspace
  if (!files) {
    return
  }

  const debug: string[] = []
  if (settings.compilerDebugLexer) debug.push('lexer')
  if (settings.compilerDebugAst) debug.push('ast')
  if (settings.compilerDebugIr) debug.push('ir')

  dispatch(newProgramStartAction())
  try {
    const { events } = await client.run(files, debug.length > 0 ? { debug } : undefined)
    for (const ev of events) {
      dispatch(newProgramWriteAction(ev))
    }
  } catch (e) {
    dispatch(newErrorAction(e instanceof Error ? e.message : String(e)))
  } finally {
    dispatch(newProgramFinishAction())
  }
}
