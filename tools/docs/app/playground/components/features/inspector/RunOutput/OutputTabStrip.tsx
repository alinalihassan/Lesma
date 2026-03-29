import React, { useCallback, useMemo } from 'react'
import { useDispatch, useSelector } from 'react-redux'
import type { Diagnostic } from 'vscode-languageserver-protocol'

import type { InspectorOutputTab, State } from '@/playground/store/state'
import { newUIStateChangeAction } from '@/playground/store'

import './OutputTabStrip.css'

function countDiagnostics(markers: Record<string, Diagnostic[] | null> | undefined): number {
  if (!markers) {
    return 0
  }
  return Object.values(markers).reduce((n, list) => n + (list?.length ?? 0), 0)
}

/**
 * Terminal / Problems tabs for the bottom panel — lives in {@link PanelHeader} left side.
 */
export const OutputTabStrip: React.FC = () => {
  const dispatch = useDispatch()
  const inspectorTab = useSelector((state: State) => state.ui?.inspectorTab)
  const markers = useSelector((state: State) => state.status?.markers)

  const activeTab: InspectorOutputTab = inspectorTab ?? 'terminal'
  const problemCount = useMemo(() => countDiagnostics(markers), [markers])

  const setTab = useCallback(
    (tab: InspectorOutputTab) => {
      dispatch(newUIStateChangeAction({ inspectorTab: tab }))
    },
    [dispatch],
  )

  return (
    <div className="OutputTabStrip" role="tablist" aria-label="Output">
      <button
        type="button"
        role="tab"
        aria-selected={activeTab === 'terminal'}
        className={activeTab === 'terminal' ? 'OutputTabStrip__tab OutputTabStrip__tab--active' : 'OutputTabStrip__tab'}
        onClick={() => setTab('terminal')}
      >
        Terminal
      </button>
      <button
        type="button"
        role="tab"
        aria-selected={activeTab === 'problems'}
        className={activeTab === 'problems' ? 'OutputTabStrip__tab OutputTabStrip__tab--active' : 'OutputTabStrip__tab'}
        onClick={() => setTab('problems')}
      >
        Problems
        {problemCount > 0 ? <span className="OutputTabStrip__count">{problemCount}</span> : null}
      </button>
    </div>
  )
}
