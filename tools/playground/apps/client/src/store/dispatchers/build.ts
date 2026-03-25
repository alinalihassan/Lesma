import client from '~/services/api'
import type { DispatchFn, StateProvider } from '~/store/helpers'
import { newProgramFinishAction, newProgramStartAction, newProgramWriteAction } from '~/store/actions/build'
import { newErrorAction } from '~/store/actions/ui'

import type { Dispatcher } from './utils'

/**
 * Runs the current workspace on the playground API (`POST /v2/run`).
 */
export const runFileDispatcher: Dispatcher = async (dispatch: DispatchFn, getState: StateProvider) => {
  const { workspace } = getState()
  const { files } = workspace
  if (!files) {
    return
  }

  dispatch(newProgramStartAction())
  try {
    const { events } = await client.run(files)
    for (const ev of events) {
      dispatch(newProgramWriteAction(ev))
    }
  } catch (e) {
    dispatch(newErrorAction(e instanceof Error ? e.message : String(e)))
  } finally {
    dispatch(newProgramFinishAction())
  }
}
