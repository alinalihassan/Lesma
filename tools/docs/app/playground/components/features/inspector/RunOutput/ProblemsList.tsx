import React, { useMemo } from 'react'
import { useDispatch, useSelector } from 'react-redux'
import { DiagnosticSeverity, type Diagnostic } from 'vscode-languageserver-protocol'

import type { State } from '@/playground/store/state'
import { newUIStateChangeAction } from '@/playground/store'
import { newFileSelectAction } from '@/playground/store/workspace/dispatchers/files'

import './ProblemsList.css'

type Row = { fileName: string; diagnostic: Diagnostic }

function flattenMarkers(markers: Record<string, Diagnostic[] | null> | undefined): Row[] {
  if (!markers) {
    return []
  }
  const rows: Row[] = []
  for (const [fileName, list] of Object.entries(markers)) {
    if (!list?.length) {
      continue
    }
    for (const diagnostic of list) {
      rows.push({ fileName, diagnostic })
    }
  }
  rows.sort((a, b) => {
    const fa = a.fileName.localeCompare(b.fileName)
    if (fa !== 0) {
      return fa
    }
    const la = a.diagnostic.range.start.line
    const lb = b.diagnostic.range.start.line
    if (la !== lb) {
      return la - lb
    }
    return a.diagnostic.range.start.character - b.diagnostic.range.start.character
  })
  return rows
}

function severityLabel(s?: DiagnosticSeverity): string {
  switch (s) {
    case DiagnosticSeverity.Error:
      return 'Error'
    case DiagnosticSeverity.Warning:
      return 'Warning'
    case DiagnosticSeverity.Information:
      return 'Info'
    case DiagnosticSeverity.Hint:
      return 'Hint'
    default:
      return 'Error'
  }
}

function severityClass(s?: DiagnosticSeverity): string {
  switch (s) {
    case DiagnosticSeverity.Warning:
      return 'ProblemsList__row--warning'
    case DiagnosticSeverity.Information:
      return 'ProblemsList__row--info'
    case DiagnosticSeverity.Hint:
      return 'ProblemsList__row--hint'
    default:
      return 'ProblemsList__row--error'
  }
}

export const ProblemsList: React.FC = () => {
  const dispatch = useDispatch()
  const markers = useSelector((s: State) => s.status?.markers)

  const rows = useMemo(() => flattenMarkers(markers), [markers])

  const onRowActivate = (fileName: string, d: Diagnostic) => {
    const line = d.range.start.line + 1
    const column = d.range.start.character + 1
    dispatch(newFileSelectAction(fileName))
    dispatch(
      newUIStateChangeAction({
        pendingEditorReveal: { path: fileName, line, column },
      }),
    )
  }

  if (rows.length === 0) {
    return (
      <div className="ProblemsList ProblemsList--empty">
        <p className="ProblemsList__empty">No problems have been detected in the workspace.</p>
      </div>
    )
  }

  return (
    <ul className="ProblemsList" role="listbox" aria-label="Problems">
      {rows.map(({ fileName, diagnostic: d }, i) => {
        const line = d.range.start.line + 1
        const col = d.range.start.character + 1
        const key = `${fileName}:${i}:${d.message}`
        return (
          <li key={key}>
            <button
              type="button"
              className={`ProblemsList__row ${severityClass(d.severity)}`}
              onClick={() => onRowActivate(fileName, d)}
            >
              <span className="ProblemsList__badge">{severityLabel(d.severity)}</span>
              <span className="ProblemsList__meta">
                {fileName}:{line}:{col}
              </span>
              <span className="ProblemsList__message">{d.message}</span>
            </button>
          </li>
        )
      })}
    </ul>
  )
}
